#Requires -Version 5.1
<#
SCons shared-cache footprint: report, warn, prune, clean.

The cache is one directory reused by this checkout and by every worktree
(tools/wt_bootstrap_env.ps1 writes SCONS_CACHE into each worktree .env), so the
numbers here cover all of them.

Path resolution order: -CachePath, then SCONS_CACHE (process env), then the
SCONS_CACHE line in the repo .env, then <repo>\.cache\scons. Before anything is
deleted the resolved path must be a literal .cache\scons directory that either
looks like an SCons cache (config / CACHEDIR.TAG present) or is empty.

Deletion is permanent by design: a recycled multi-GiB cache still occupies the
disk, which is the whole reason to clean it.

Actions
  Status  print size, file count, age and the configured limits
  Check   print nothing when under the soft limit; warn above it (exit 1) and
          above the hard limit (exit 2). This is what build_remote.ps1 calls.
  Prune   delete the oldest entries in place until the cache fits -MaxMiB
  Clean   rename the cache aside atomically, recreate it empty, then delete the
          old tree (a build can never observe a half-deleted cache)

Refuses to touch anything while an scons/cl/link process is alive unless -Force;
-WaitSeconds waits for the machine to go idle first. There is no kernel lock in
the compiler path (deliberate: parallel worktree builds must stay allowed), so
this process scan is the exclusion mechanism.

Exit codes: 0 ok, 1 soft limit, 2 hard limit, 3 build active, 4 bad path,
5 deletion incomplete.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [ValidateSet('Status', 'Check', 'Prune', 'Clean')]
    [string]$Action = 'Status',

    # Explicit cache directory. Empty = resolve from env/.env/default.
    [string]$CachePath = '',

    # Prune target. 0 = use the soft limit.
    [int]$MaxMiB = 0,

    # Limit overrides (MiB). 0 = the constants at the top of this file.
    [int]$WarnMiB = 0,
    [int]$HardMiB = 0,

    # Wait up to N seconds for running builds to finish instead of refusing.
    [int]$WaitSeconds = 0,

    [switch]$Json,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$Script:DefaultWarnMiB = 1200   # ~1.2 GiB
$Script:DefaultHardMiB = 2000   # ~2.0 GiB
$Script:GenerationMiB = 411     # measured: one template_debug/dev/tests generation

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path.TrimEnd('\', '/')

function Read-DotEnvValue {
    param([string]$Name)

    $envFile = Join-Path $repoRoot '.env'
    if (-not (Test-Path -LiteralPath $envFile -PathType Leaf)) { return '' }
    try {
        foreach ($line in [System.IO.File]::ReadAllLines($envFile, [System.Text.UTF8Encoding]::new($false))) {
            $trimmed = $line.Trim()
            if ($trimmed.Length -eq 0) { continue }
            if ($trimmed.StartsWith('#')) { continue }
            $eq = $trimmed.IndexOf('=')
            if ($eq -lt 1) { continue }
            if ($trimmed.Substring(0, $eq).Trim() -ne $Name) { continue }
            return $trimmed.Substring($eq + 1).Trim().Trim('"').Trim("'")
        }
    }
    catch {
        return ''
    }
    return ''
}

function Resolve-CacheDirectory {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        return [pscustomobject]@{ Path = $ExplicitPath; Source = '-CachePath' }
    }
    if ($env:SCONS_CACHE) {
        return [pscustomobject]@{ Path = $env:SCONS_CACHE; Source = 'SCONS_CACHE (environment)' }
    }
    $fromDotEnv = Read-DotEnvValue -Name 'SCONS_CACHE'
    if ($fromDotEnv) {
        return [pscustomobject]@{ Path = $fromDotEnv; Source = '.env' }
    }
    return [pscustomobject]@{ Path = (Join-Path $repoRoot '.cache\scons'); Source = 'default' }
}

function Assert-CacheDirectory {
    param([string]$Path)

    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        Write-Host "ERROR: cache path is not absolute: $Path" -ForegroundColor Red
        exit 4
    }

    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $leaf = Split-Path -Leaf $full
    $parentLeaf = Split-Path -Leaf (Split-Path -Parent $full)

    if ($leaf -ne 'scons') {
        Write-Host "ERROR: refusing '$full' - the directory name must be exactly 'scons'." -ForegroundColor Red
        exit 4
    }
    if ($parentLeaf -ne '.cache') {
        Write-Host "ERROR: refusing '$full' - it must live directly under a '.cache' directory." -ForegroundColor Red
        exit 4
    }
    if ($full -eq $repoRoot.TrimEnd('\', '/')) {
        Write-Host "ERROR: refusing '$full' - that is the repository root." -ForegroundColor Red
        exit 4
    }

    if (Test-Path -LiteralPath $full -PathType Leaf) {
        Write-Host "ERROR: '$full' is a file, not a directory." -ForegroundColor Red
        exit 4
    }

    if (Test-Path -LiteralPath $full -PathType Container) {
        $hasConfig = Test-Path -LiteralPath (Join-Path $full 'config') -PathType Leaf
        $hasTag = Test-Path -LiteralPath (Join-Path $full 'CACHEDIR.TAG') -PathType Leaf
        $isEmpty = @([System.IO.Directory]::EnumerateFileSystemEntries($full)).Count -eq 0
        if (-not $hasConfig -and -not $hasTag -and -not $isEmpty) {
            Write-Host "ERROR: refusing '$full' - it is not empty and has neither 'config' nor 'CACHEDIR.TAG', so it does not look like an SCons cache." -ForegroundColor Red
            exit 4
        }
    }

    return $full
}

function Measure-CacheDirectory {
    param([string]$Path)

    $files = 0
    [long]$bytes = 0
    $oldest = [datetime]::MaxValue
    $newest = [datetime]::MinValue

    if (Test-Path -LiteralPath $Path -PathType Container) {
        foreach ($file in [System.IO.Directory]::EnumerateFiles($Path, '*', [System.IO.SearchOption]::AllDirectories)) {
            try {
                $info = [System.IO.FileInfo]::new($file)
                $files++
                $bytes += $info.Length
                if ($info.LastWriteTime -lt $oldest) { $oldest = $info.LastWriteTime }
                if ($info.LastWriteTime -gt $newest) { $newest = $info.LastWriteTime }
            }
            catch {
                Write-Verbose ("cache entry vanished during the scan: " + $file)
            }
        }
    }

    return [pscustomobject]@{
        Files  = $files
        Bytes  = $bytes
        MiB    = [math]::Round($bytes / 1MB, 1)
        Oldest = $oldest
        Newest = $newest
    }
}

function Get-ActiveBuildProcess {
    $candidates = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe' OR Name='scons.exe' OR Name='cl.exe' OR Name='link.exe'" -ErrorAction SilentlyContinue)
    $live = New-Object System.Collections.Generic.List[object]
    foreach ($proc in $candidates) {
        $isCompiler = ($proc.Name -eq 'cl.exe' -or $proc.Name -eq 'link.exe' -or $proc.Name -eq 'scons.exe')
        $isScons = ($proc.CommandLine -and $proc.CommandLine -match 'scons')
        if ($isCompiler -or $isScons) {
            $live.Add($proc)
        }
    }
    return $live.ToArray()
}

function Wait-ForIdleBuild {
    param([int]$Seconds)

    $deadline = (Get-Date).AddSeconds([Math]::Max($Seconds, 0))
    do {
        $live = @(Get-ActiveBuildProcess)
        if ($live.Count -eq 0) { return @() }
        if ($Seconds -le 0) { return $live }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)

    return @(Get-ActiveBuildProcess)
}

$resolved = Resolve-CacheDirectory -ExplicitPath $CachePath
$cacheDir = Assert-CacheDirectory -Path $resolved.Path
$stats = Measure-CacheDirectory -Path $cacheDir

# Effective limits: the -WarnMiB/-HardMiB parameters, otherwise the constants at
# the top of this file. Deliberately NOT read from the environment or .env: this
# workspace's IDE injects .env into every terminal, so an env-driven limit could
# change between sessions without anyone editing the script.
$softMiB = $Script:DefaultWarnMiB
if ($WarnMiB -gt 0) {
    $softMiB = $WarnMiB
}

$hardMiB = $Script:DefaultHardMiB
if ($HardMiB -gt 0) {
    $hardMiB = $HardMiB
}

if ($hardMiB -lt $softMiB) { $hardMiB = $softMiB }

$state = 'ok'
if ($stats.MiB -ge $hardMiB) {
    $state = 'hard'
}
elseif ($stats.MiB -ge $softMiB) {
    $state = 'soft'
}

$generationRatio = $stats.MiB / $Script:GenerationMiB
if ($stats.MiB -gt 0 -and $generationRatio -lt 0.1) {
    $genText = '<0.1'
}
else {
    $genText = [string][math]::Round($generationRatio, 1)
}

function Write-CacheSummary {
    Write-Host ("[cache] path : " + $cacheDir + "  (from " + $resolved.Source + ")")
    Write-Host ("[cache] size : " + $stats.MiB + " MiB in " + $stats.Files + " files, ~" + $genText + " generation(s) of " + $Script:GenerationMiB + " MiB")
    if ($stats.Files -gt 0) {
        Write-Host ("[cache] age  : " + $stats.Oldest.ToString('yyyy-MM-dd HH:mm') + " -> " + $stats.Newest.ToString('yyyy-MM-dd HH:mm'))
    }
    Write-Host ("[cache] limits: soft " + $softMiB + " MiB, hard " + $hardMiB + " MiB (defaults in this script; -WarnMiB/-HardMiB to override)")
}

function Write-CacheWarning {
    Write-Host ("[cache] WARNING: the SCons cache is " + $stats.MiB + " MiB in " + $stats.Files + " files (~" + $genText + " builds), above the " + $softMiB + " MiB soft limit.") -ForegroundColor Yellow
    Write-Host ("[cache] SCons never evicts cache entries; only manual pruning bounds it.") -ForegroundColor Yellow
    Write-Host ("[cache] Free space now: prune to the limit, or reset the cache entirely:") -ForegroundColor Yellow
    Write-Host ("[cache]   powershell -File tools/scons_cache.ps1 -Action Prune -MaxMiB " + $softMiB)
    Write-Host ("[cache]   powershell -File tools/scons_cache.ps1 -Action Clean")
    if ($state -eq 'hard') {
        Write-Host ("[cache] HARD LIMIT REACHED (" + $hardMiB + " MiB): clean the cache before the next build.") -ForegroundColor Red
    }
}

if ($Json) {
    $payload = [pscustomobject]@{
        action     = $Action
        path       = $cacheDir
        source     = $resolved.Source
        files      = $stats.Files
        bytes      = $stats.Bytes
        mib        = $stats.MiB
        soft_limit = $softMiB
        hard_limit = $hardMiB
        state      = $state
    }
    Write-Output ($payload | ConvertTo-Json -Compress)
}

switch ($Action) {
    'Status' {
        if (-not $Json) { Write-CacheSummary }
        exit 0
    }

    'Check' {
        if (-not $Json) {
            if ($state -eq 'hard' -or $state -eq 'soft') {
                Write-CacheWarning
            }
            elseif ($VerbosePreference -ne 'SilentlyContinue') {
                Write-CacheSummary
            }
        }
        if ($state -eq 'hard') { exit 2 }
        if ($state -eq 'soft') { exit 1 }
        exit 0
    }

    'Prune' {
        $targetMiB = $MaxMiB
        if ($targetMiB -le 0) { $targetMiB = $softMiB }

        if (-not $Json) { Write-CacheSummary; Write-Host ("[cache] prune target: " + $targetMiB + " MiB") }

        if ($stats.MiB -le $targetMiB) {
            if (-not $Json) { Write-Host ("[cache] already under the target; nothing to prune.") }
            exit 0
        }

        if (-not $Force) {
            $live = @(Wait-ForIdleBuild -Seconds $WaitSeconds)
            if ($live.Count -gt 0) {
                if (-not $Json) {
                    Write-Host ("[cache] REFUSED: " + $live.Count + " build process(es) are running:") -ForegroundColor Red
                    foreach ($proc in $live) { Write-Host ("[cache]   PID " + $proc.ProcessId + " " + $proc.Name + " :: " + $proc.CommandLine) }
                }
                exit 3
            }
        }

        $entries = New-Object System.Collections.Generic.List[object]
        foreach ($file in [System.IO.Directory]::EnumerateFiles($cacheDir, '*', [System.IO.SearchOption]::AllDirectories)) {
            if ([System.IO.Path]::GetDirectoryName($file) -eq $cacheDir) { continue }  # keep config/CACHEDIR.TAG/signature
            try { $entries.Add([System.IO.FileInfo]::new($file)) } catch { Write-Verbose ("cache entry vanished during the scan: " + $file) }
        }

        [long]$remaining = $stats.Bytes
        $targetBytes = [long]$targetMiB * 1MB
        $deleted = 0
        [long]$freed = 0

        foreach ($entry in ($entries | Sort-Object LastWriteTime)) {
            if ($remaining -le $targetBytes) { break }
            if ($PSCmdlet.ShouldProcess($entry.FullName, 'Delete cache entry')) {
                try {
                    $length = $entry.Length
                    [System.IO.File]::Delete($entry.FullName)
                    $remaining -= $length
                    $freed += $length
                    $deleted++
                }
                catch {
                    Write-Verbose ("could not delete " + $entry.FullName + ": " + $_.Exception.Message)
                }
            }
        }

        $after = Measure-CacheDirectory -Path $cacheDir
        if (-not $Json) {
            Write-Host ("[cache] pruned " + $deleted + " entr(ies), freed " + [math]::Round($freed / 1MB, 1) + " MiB; now " + $after.MiB + " MiB in " + $after.Files + " files")
        }
        if ($after.MiB -gt $targetBytes / 1MB) { exit 5 }
        exit 0
    }

    'Clean' {
        if (-not $Json) { Write-CacheSummary }

        if (-not $Force) {
            $live = @(Wait-ForIdleBuild -Seconds $WaitSeconds)
            if ($live.Count -gt 0) {
                if (-not $Json) {
                    Write-Host ("[cache] REFUSED: " + $live.Count + " build process(es) are running, so deleting the cache could make a build fail mid-flight:") -ForegroundColor Red
                    foreach ($proc in $live) { Write-Host ("[cache]   PID " + $proc.ProcessId + " " + $proc.Name + " :: " + $proc.CommandLine) }
                    Write-Host ("[cache] Retry when idle, or pass -WaitSeconds N to queue behind the build.") -ForegroundColor Yellow
                }
                exit 3
            }
        }

        if (-not (Test-Path -LiteralPath $cacheDir -PathType Container)) {
            if (-not $Json) { Write-Host '[cache] nothing to clean.' }
            exit 0
        }

        $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
        $trash = Join-Path (Split-Path -Parent $cacheDir) ('scons.trash-' + $stamp)
        $suffix = 1
        while (Test-Path -LiteralPath $trash) {
            $trash = Join-Path (Split-Path -Parent $cacheDir) ('scons.trash-' + $stamp + '-' + $suffix)
            $suffix++
        }

        if (-not $PSCmdlet.ShouldProcess($cacheDir, 'Permanently delete the SCons cache and recreate it empty')) {
            exit 0
        }

        # Rename first: atomic, instant, and a build can never observe a partly
        # deleted cache. Everything below the rename happens outside the window.
        [System.IO.Directory]::Move($cacheDir, $trash)
        [System.IO.Directory]::CreateDirectory($cacheDir) | Out-Null

        # Preserve the cache's identity/metadata so SCons finds the same layout and
        # the build-signature delta diagnostics keep their baseline.
        foreach ($name in @('config', 'CACHEDIR.TAG', 'build-signature.json')) {
            $source = Join-Path $trash $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                [System.IO.File]::Copy($source, (Join-Path $cacheDir $name), $true)
            }
        }

        $deleted = $false
        $lastError = ''
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            try {
                [System.IO.Directory]::Delete($trash, $true)
                $deleted = $true
                break
            }
            catch {
                $lastError = $_.Exception.Message
                Start-Sleep -Milliseconds 500
            }
        }

        if (-not $deleted) {
            Write-Host ("[cache] WARNING: recreated the cache, but could not remove the old tree: " + $trash) -ForegroundColor Yellow
            Write-Host ("[cache] cause: " + $lastError) -ForegroundColor Yellow
            Write-Host ("[cache] retry: powershell -File tools/scons_cache.ps1 -Action Status  (then delete that folder by hand)") -ForegroundColor Yellow
            exit 5
        }

        $after = Measure-CacheDirectory -Path $cacheDir
        if (-not $Json) {
            Write-Host ("[cache] cleaned: freed " + $stats.MiB + " MiB (" + $stats.Files + " files); now " + $after.MiB + " MiB in " + $after.Files + " files")
        }
        exit 0
    }
}
