#Requires -Version 5.1
<#
SCons shared-cache footprint: report, warn, prune, clean, and selectively trim worktrees.

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
  Status        print size, file count, age, ledger counts, and configured limits
  Check         print nothing when under the soft limit; warn above it (exit 1) and
                above the hard limit (exit 2). This is what build_remote.ps1 calls.
  Prune         delete the oldest entries in place until the cache fits -MaxMiB
  Clean         rename the cache aside atomically, recreate it empty, then delete the
                old tree (a build can never observe a half-deleted cache)
  TrimWorktree  selectively delete cache entries exclusive to -WorktreePath,
                handing over clean landed commits to the primary checkout
  TrimOrphans   sweep ledgers for removed worktrees, superseded intermediate keys,
                expired handover pins, untracked unlinked payload entries, and
                optionally enforce -MaxMiB

Refuses to touch anything while an scons/cl/link process is alive unless -Force;
-WaitSeconds waits for the machine to go idle first. There is no kernel lock in
the compiler path (deliberate: parallel worktree builds must stay allowed), so
this process scan is the exclusion mechanism.

Exit codes: 0 ok, 1 soft limit, 2 hard limit, 3 build active, 4 bad path,
5 deletion incomplete.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [ValidateSet('Status', 'Check', 'Prune', 'Clean', 'TrimWorktree', 'TrimOrphans')]
    [string]$Action = 'Status',

    # Explicit cache directory. Empty = resolve from env/.env/default.
    [string]$CachePath = '',

    # Worktree path for TrimWorktree.
    [string]$WorktreePath = '',

    # Hours to retain landed-commit keys in _handover.json before expiry.
    [int]$HandoverHours = 24,

    # Prune target. 0 = use the soft limit (for Prune) or no cap (for TrimOrphans).
    [int]$MaxMiB = 0,

    # Limit overrides (MiB). 0 = the constants at the top of this file.
    [int]$WarnMiB = 0,
    [int]$HardMiB = 0,

    # Wait up to N seconds for running builds to finish instead of refusing.
    [int]$WaitSeconds = 0,

    # If a build is active during TrimWorktree/TrimOrphans, exit 0 quietly instead of exit 3.
    [switch]$QuietIfBusy,

    [switch]$Json,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$Script:DefaultWarnMiB = 1200   # ~1.2 GiB
$Script:DefaultHardMiB = 2000   # ~2.0 GiB
$Script:GenerationMiB = 411     # measured: one template_debug/dev/tests generation

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path.TrimEnd('\', '/')

if (-not ('SconsCacheLinkInfo' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class SconsCacheLinkInfo {
    [StructLayout(LayoutKind.Sequential)]
    private struct BY_HANDLE_FILE_INFORMATION {
        public uint FileAttributes;
        public uint CreationTimeLow;
        public uint CreationTimeHigh;
        public uint LastAccessTimeLow;
        public uint LastAccessTimeHigh;
        public uint LastWriteTimeLow;
        public uint LastWriteTimeHigh;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFileW(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile
    );

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle hFile,
        out BY_HANDLE_FILE_INFORMATION lpFileInformation
    );

    public static int GetLinkCount(string path) {
        using (SafeFileHandle handle = CreateFileW(path, 0x80u, 0x7u, IntPtr.Zero, 3u, 0x02000000u, IntPtr.Zero)) {
            if (handle.IsInvalid) {
                return -1;
            }
            BY_HANDLE_FILE_INFORMATION info;
            if (!GetFileInformationByHandle(handle, out info)) {
                return -1;
            }
            return (int)info.NumberOfLinks;
        }
    }
}
'@
}

function Get-FileHardLinkCount {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return 0
    }
    try {
        $count = [SconsCacheLinkInfo]::GetLinkCount($Path)
        if ($count -lt 1) { return 1 }
        return $count
    }
    catch {
        return 1
    }
}

function Get-CanonicalPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

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

    $full = Get-CanonicalPath -Path $Path
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
    if ($full -eq (Get-CanonicalPath -Path $repoRoot)) {
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

function Test-IsCacheBucketFile {
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$FilePath
    )

    $parent = [System.IO.Path]::GetDirectoryName($FilePath)
    if (-not $parent) { return $false }
    $grandParent = [System.IO.Path]::GetDirectoryName($parent)
    if (-not $grandParent) { return $false }
    if ($grandParent.TrimEnd('\', '/') -ine $CacheRoot.TrimEnd('\', '/')) { return $false }
    $bucketName = [System.IO.Path]::GetFileName($parent)
    return ($bucketName -match '^[0-9A-Fa-f]{2}$')
}

function Get-CachePayloadFile {
    param([Parameter(Mandatory = $true)][string]$CacheRoot)

    $list = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    if (-not (Test-Path -LiteralPath $CacheRoot -PathType Container)) {
        return , $list.ToArray()
    }
    foreach ($dir in [System.IO.Directory]::EnumerateDirectories($CacheRoot)) {
        $dirName = [System.IO.Path]::GetFileName($dir)
        if ($dirName -notmatch '^[0-9A-Fa-f]{2}$') { continue }
        foreach ($file in [System.IO.Directory]::EnumerateFiles($dir)) {
            try {
                $list.Add([System.IO.FileInfo]::new($file))
            }
            catch {
                Write-Verbose ("cache entry vanished during scan: " + $file)
            }
        }
    }
    return $list.ToArray()
}

function Measure-CacheDirectory {
    param([string]$Path)

    $files = 0
    [long]$bytes = 0
    $oldest = [datetime]::MaxValue
    $newest = [datetime]::MinValue

    foreach ($info in @(Get-CachePayloadFile -CacheRoot $Path)) {
        $files++
        $bytes += $info.Length
        if ($info.LastWriteTime -lt $oldest) { $oldest = $info.LastWriteTime }
        if ($info.LastWriteTime -gt $newest) { $newest = $info.LastWriteTime }
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

function Get-PrimaryRepoRoot {
    param([Parameter(Mandatory = $true)][string]$StartRoot)

    try {
        $commonDir = (& git -C $StartRoot rev-parse --git-common-dir 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and $commonDir) {
            if (-not [System.IO.Path]::IsPathRooted($commonDir)) {
                $commonDir = Join-Path $StartRoot $commonDir
            }
            $canonCommon = Get-CanonicalPath -Path $commonDir
            if ([System.IO.Path]::GetFileName($canonCommon) -ieq '.git') {
                return Get-CanonicalPath -Path (Split-Path -Parent $canonCommon)
            }
        }
    }
    catch {
        Write-Verbose "git rev-parse --git-common-dir failed: $($_.Exception.Message)"
    }
    return Get-CanonicalPath -Path $StartRoot
}

function Get-LiveWorktreeSet {
    param([Parameter(Mandatory = $true)][string]$PrimaryRoot)

    $set = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    try {
        $lines = @(& git -C $PrimaryRoot worktree list --porcelain 2>$null)
        if ($LASTEXITCODE -ne 0) {
            return $null
        }
        foreach ($line in $lines) {
            if ($line -match '^worktree\s+(.+)$') {
                $wtPath = $matches[1].Trim()
                if ($wtPath -and (Test-Path -LiteralPath $wtPath -PathType Container)) {
                    [void]$set.Add((Get-CanonicalPath -Path $wtPath))
                }
            }
        }
    }
    catch {
        return $null
    }
    if ($set.Count -eq 0) {
        return $null
    }
    return $set
}

function Read-JsonFileUtf8 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try {
        $raw = [System.IO.File]::ReadAllText($Path, [System.Text.UTF8Encoding]::new($false))
        if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
        return ($raw | ConvertFrom-Json)
    }
    catch {
        return $null
    }
}

function Save-JsonFileUtf8 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Object
    )

    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    $jsonText = ($Object | ConvertTo-Json -Depth 8) + "`n"
    $tmpPath = "$Path.tmp.$PID"
    try {
        [System.IO.File]::WriteAllText($tmpPath, $jsonText, [System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::Replace($tmpPath, $Path, [NullString]::Value)
    }
    catch {
        [System.IO.File]::WriteAllText($Path, $jsonText, [System.Text.UTF8Encoding]::new($false))
    }
    finally {
        if (Test-Path -LiteralPath $tmpPath -PathType Leaf) {
            try { [System.IO.File]::Delete($tmpPath) } catch { Write-Verbose "tmp cleanup failed: $tmpPath" }
        }
    }
}

function Get-CacheFilePath {
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$Sig
    )

    if ($Sig -notmatch '^[0-9A-Fa-f]{32,}$') {
        return ''
    }
    $bucket = $Sig.Substring(0, 2).ToUpperInvariant()
    return (Join-Path (Join-Path $CacheRoot $bucket) $Sig)
}

function Get-WorktreeLedger {
    param([Parameter(Mandatory = $true)][string]$CacheRoot)

    $ledgerDir = Join-Path $CacheRoot '.worktrees'
    $list = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $ledgerDir -PathType Container)) {
        return , $list.ToArray()
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $ledgerDir -Filter '*.json' -File -ErrorAction SilentlyContinue)) {
        if ($file.Name -eq '_handover.json') { continue }
        $data = Read-JsonFileUtf8 -Path $file.FullName
        if (-not $data) { continue }
        $wtPath = if ($data.worktree_path) { Get-CanonicalPath -Path ([string]$data.worktree_path) } else { '' }
        $activeTargets = @{}
        if ($data.active_targets) {
            foreach ($prop in $data.active_targets.PSObject.Properties) {
                $activeTargets[$prop.Name] = [string]$prop.Value
            }
        }
        $pushedKeys = New-Object System.Collections.Generic.List[string]
        if ($data.pushed_keys) {
            foreach ($k in @($data.pushed_keys)) {
                if ($k) { $pushedKeys.Add([string]$k) }
            }
        }
        $list.Add([pscustomobject]@{
            FilePath      = $file.FullName
            FileName      = $file.Name
            WorktreeId    = [string]$data.worktree_id
            WorktreePath  = $wtPath
            IsPrimary     = [bool]$data.is_primary
            Branch        = [string]$data.branch
            HeadCommit    = [string]$data.head_commit
            IsDirty       = [bool]$data.is_dirty
            UpdatedAt     = [string]$data.updated_at
            ActiveTargets = $activeTargets
            PushedKeys    = $pushedKeys
            Raw           = $data
        })
    }
    return $list.ToArray()
}

function Read-HandoverState {
    param([Parameter(Mandatory = $true)][string]$CacheRoot)

    $handoverPath = Join-Path (Join-Path $CacheRoot '.worktrees') '_handover.json'
    $activePins = @{}
    $expiredKeys = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
    $nowUtc = [datetime]::UtcNow
    $changed = $false

    $data = Read-JsonFileUtf8 -Path $handoverPath
    if ($data -and $data.pins) {
        foreach ($prop in $data.pins.PSObject.Properties) {
            $sig = $prop.Name
            $meta = $prop.Value
            $isExpired = $false
            if ($meta -and $meta.expires_at) {
                try {
                    $exp = [datetime]::Parse([string]$meta.expires_at, $null, [System.Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
                    if ($exp -le $nowUtc) { $isExpired = $true }
                }
                catch {
                    $isExpired = $false
                }
            }
            if ($isExpired) {
                [void]$expiredKeys.Add($sig)
                $changed = $true
            }
            else {
                $activePins[$sig] = $meta
            }
        }
    }

    return [pscustomobject]@{
        Path        = $handoverPath
        ActivePins  = $activePins
        ExpiredKeys = $expiredKeys
        Changed     = $changed
    }
}

function Save-HandoverState {
    param(
        [Parameter(Mandatory = $true)][string]$HandoverPath,
        [Parameter(Mandatory = $true)][hashtable]$Pins
    )

    if ($Pins.Count -eq 0) {
        if (Test-Path -LiteralPath $HandoverPath -PathType Leaf) {
            try { [System.IO.File]::Delete($HandoverPath) } catch { Write-Verbose "could not delete $HandoverPath" }
        }
        return
    }

    $payload = [ordered]@{
        schema = 1
        pins   = $Pins
    }
    Save-JsonFileUtf8 -Path $HandoverPath -Object $payload
}

function Test-IsCommitLandedInPrimary {
    param(
        [Parameter(Mandatory = $true)][string]$PrimaryRoot,
        [string]$Commit
    )

    if (-not $Commit -or $Commit -notmatch '^[0-9a-fA-F]{40}$') {
        return $false
    }
    try {
        & git -C $PrimaryRoot merge-base --is-ancestor $Commit HEAD 2>$null | Out-Null
        return ($LASTEXITCODE -eq 0)
    }
    catch {
        return $false
    }
}

function Invoke-DeleteExclusiveKey {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$Sig,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.HashSet[string]]$ProcessedKeys,
        [ref]$DeletedCount,
        [ref]$FreedBytes,
        [ref]$SkippedHardlinked
    )

    if (-not $ProcessedKeys.Add($Sig)) { return $false }
    $filePath = Get-CacheFilePath -CacheRoot $CacheRoot -Sig $Sig
    if (-not $filePath -or -not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        return $true
    }
    $linkCount = Get-FileHardLinkCount -Path $filePath
    if ($linkCount -gt 1) {
        $SkippedHardlinked.Value++
        return $false
    }
    if ($PSCmdlet.ShouldProcess($filePath, 'Delete exclusive worktree cache entry')) {
        try {
            $info = [System.IO.FileInfo]::new($filePath)
            $length = $info.Length
            [System.IO.File]::Delete($filePath)
            $DeletedCount.Value++
            $FreedBytes.Value += $length
            return $true
        }
        catch {
            Write-Verbose ("could not delete " + $filePath + ": " + $_.Exception.Message)
            return $false
        }
    }
    return $false
}

$resolved = Resolve-CacheDirectory -ExplicitPath $CachePath
$cacheDir = Assert-CacheDirectory -Path $resolved.Path
$stats = Measure-CacheDirectory -Path $cacheDir
$primaryRoot = Get-PrimaryRepoRoot -StartRoot $repoRoot

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

function Get-LedgerCount {
    $ledgers = @(Get-WorktreeLedger -CacheRoot $cacheDir)
    $liveSet = Get-LiveWorktreeSet -PrimaryRoot $primaryRoot
    $liveCount = 0
    $deadCount = 0
    foreach ($ledger in $ledgers) {
        if ($liveSet -and $ledger.WorktreePath -and $liveSet.Contains($ledger.WorktreePath)) {
            $liveCount++
        }
        elseif ($liveSet) {
            $deadCount++
        }
        elseif ($ledger.WorktreePath -and (Test-Path -LiteralPath $ledger.WorktreePath -PathType Container)) {
            $liveCount++
        }
        else {
            $deadCount++
        }
    }
    $handover = Read-HandoverState -CacheRoot $cacheDir
    return [pscustomobject]@{
        Total        = $ledgers.Count
        Live         = $liveCount
        Dead         = $deadCount
        HandoverPins = $handover.ActivePins.Count
    }
}

function Write-CacheSummary {
    param([object]$LedgerCounts = $null)

    Write-Host ("[cache] path : " + $cacheDir + "  (from " + $resolved.Source + ")")
    Write-Host ("[cache] size : " + $stats.MiB + " MiB in " + $stats.Files + " files, ~" + $genText + " generation(s) of " + $Script:GenerationMiB + " MiB")
    if ($stats.Files -gt 0) {
        Write-Host ("[cache] age  : " + $stats.Oldest.ToString('yyyy-MM-dd HH:mm') + " -> " + $stats.Newest.ToString('yyyy-MM-dd HH:mm'))
    }
    if ($LedgerCounts -and ($LedgerCounts.Total -gt 0 -or $LedgerCounts.HandoverPins -gt 0)) {
        Write-Host ("[cache] worktrees: " + $LedgerCounts.Live + " live, " + $LedgerCounts.Dead + " dead ledger(s), " + $LedgerCounts.HandoverPins + " handover pin(s)")
        if ($LedgerCounts.Dead -gt 0) {
            Write-Host "[cache] hint : run 'powershell -File tools/scons_cache.ps1 -Action TrimOrphans' to reclaim dead worktree entries." -ForegroundColor Yellow
        }
    }
    Write-Host ("[cache] limits: soft " + $softMiB + " MiB, hard " + $hardMiB + " MiB (defaults in this script; -WarnMiB/-HardMiB to override)")
}

function Write-CacheWarning {
    Write-Host ("[cache] WARNING: the SCons cache is " + $stats.MiB + " MiB in " + $stats.Files + " files (~" + $genText + " builds), above the " + $softMiB + " MiB soft limit.") -ForegroundColor Yellow
    Write-Host ("[cache] SCons never evicts cache entries; only manual pruning bounds it.") -ForegroundColor Yellow
    Write-Host ("[cache] Free space now: trim dead worktrees, prune to the limit, or reset the cache:") -ForegroundColor Yellow
    Write-Host ("[cache]   powershell -File tools/scons_cache.ps1 -Action TrimOrphans -MaxMiB " + $softMiB)
    Write-Host ("[cache]   powershell -File tools/scons_cache.ps1 -Action Prune -MaxMiB " + $softMiB)
    Write-Host ("[cache]   powershell -File tools/scons_cache.ps1 -Action Clean")
    if ($state -eq 'hard') {
        Write-Host ("[cache] HARD LIMIT REACHED (" + $hardMiB + " MiB): clean the cache before the next build.") -ForegroundColor Red
    }
}

switch ($Action) {
    'Status' {
        $ledgerCounts = Get-LedgerCount
        if ($Json) {
            $payload = [pscustomobject]@{
                action        = $Action
                path          = $cacheDir
                source        = $resolved.Source
                files         = $stats.Files
                bytes         = $stats.Bytes
                mib           = $stats.MiB
                soft_limit    = $softMiB
                hard_limit    = $hardMiB
                state         = $state
                ledgers_live  = $ledgerCounts.Live
                ledgers_dead  = $ledgerCounts.Dead
                handover_pins = $ledgerCounts.HandoverPins
            }
            Write-Output ($payload | ConvertTo-Json -Compress)
        }
        else {
            Write-CacheSummary -LedgerCounts $ledgerCounts
        }
        exit 0
    }

    'Check' {
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
        else {
            if ($state -eq 'hard' -or $state -eq 'soft') {
                Write-CacheWarning
            }
            elseif ($VerbosePreference -ne 'SilentlyContinue') {
                Write-CacheSummary -LedgerCounts (Get-LedgerCount)
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
            if ($Json) {
                $payload = [pscustomobject]@{
                    action = $Action; path = $cacheDir; files = $stats.Files; bytes = $stats.Bytes; mib = $stats.MiB; deleted = 0; freed_mib = 0.0; target_mib = $targetMiB
                }
                Write-Output ($payload | ConvertTo-Json -Compress)
            }
            else {
                Write-Host ("[cache] already under the target; nothing to prune.")
            }
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

        $entries = @(Get-CachePayloadFile -CacheRoot $cacheDir)
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
        if ($Json) {
            $payload = [pscustomobject]@{
                action = $Action; path = $cacheDir; files = $after.Files; bytes = $after.Bytes; mib = $after.MiB; deleted = $deleted; freed_mib = [math]::Round($freed / 1MB, 1); target_mib = $targetMiB
            }
            Write-Output ($payload | ConvertTo-Json -Compress)
        }
        else {
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
        # the build-signature delta diagnostics keep their baseline. Move (not
        # Copy) so cleaning works even when the volume has zero free space.
        $preserveFiles = @('config', 'CACHEDIR.TAG')
        foreach ($sigFile in @(Get-ChildItem -LiteralPath $trash -Filter 'build-signature*.json' -File -ErrorAction SilentlyContinue)) {
            $preserveFiles += $sigFile.Name
        }
        foreach ($name in $preserveFiles) {
            $source = Join-Path $trash $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                $dest = Join-Path $cacheDir $name
                if (Test-Path -LiteralPath $dest -PathType Leaf) {
                    [System.IO.File]::Delete($dest)
                }
                [System.IO.File]::Move($source, $dest)
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
        if ($Json) {
            $payload = [pscustomobject]@{
                action = $Action; path = $cacheDir; files = $after.Files; bytes = $after.Bytes; mib = $after.MiB; freed_mib = $stats.MiB
            }
            Write-Output ($payload | ConvertTo-Json -Compress)
        }
        else {
            Write-Host ("[cache] cleaned: freed " + $stats.MiB + " MiB (" + $stats.Files + " files); now " + $after.MiB + " MiB in " + $after.Files + " files")
        }
        exit 0
    }

    'TrimWorktree' {
        if ([string]::IsNullOrWhiteSpace($WorktreePath)) {
            Write-Host "ERROR: -WorktreePath is required for -Action TrimWorktree." -ForegroundColor Red
            exit 4
        }

        $targetCanon = Get-CanonicalPath -Path $WorktreePath
        if ($targetCanon -ieq $primaryRoot) {
            Write-Host "ERROR: refusing to trim primary checkout '$targetCanon' as a collapsed worktree." -ForegroundColor Red
            exit 4
        }

        if (-not $Force) {
            $live = @(Wait-ForIdleBuild -Seconds $WaitSeconds)
            if ($live.Count -gt 0) {
                if ($QuietIfBusy) {
                    if (-not $Json) {
                        Write-Host "[cache] TrimWorktree skipped (build active); run -Action TrimOrphans when idle." -ForegroundColor DarkGray
                    }
                    else {
                        Write-Output (([pscustomobject]@{ action = $Action; path = $cacheDir; skipped_busy = $true }) | ConvertTo-Json -Compress)
                    }
                    exit 0
                }
                if (-not $Json) {
                    Write-Host ("[cache] REFUSED: " + $live.Count + " build process(es) are running:") -ForegroundColor Red
                    foreach ($proc in $live) { Write-Host ("[cache]   PID " + $proc.ProcessId + " " + $proc.Name + " :: " + $proc.CommandLine) }
                }
                exit 3
            }
        }

        $ledgerDir = Join-Path $cacheDir '.worktrees'
        if (-not (Test-Path -LiteralPath $ledgerDir -PathType Container)) {
            if ($Json) {
                Write-Output (([pscustomobject]@{ action = $Action; path = $cacheDir; worktree = $targetCanon; matched_ledgers = 0; deleted = 0; freed_mib = 0.0; handover_pinned = 0; skipped_hardlinked = 0 }) | ConvertTo-Json -Compress)
            }
            else {
                Write-Host "[cache] no worktree ledgers recorded yet; nothing to trim."
            }
            exit 0
        }

        $allLedgers = @(Get-WorktreeLedger -CacheRoot $cacheDir)
        $targetLedgers = @($allLedgers | Where-Object { $_.WorktreePath -ieq $targetCanon })
        if ($targetLedgers.Count -eq 0) {
            if ($Json) {
                Write-Output (([pscustomobject]@{ action = $Action; path = $cacheDir; worktree = $targetCanon; matched_ledgers = 0; deleted = 0; freed_mib = 0.0; handover_pinned = 0; skipped_hardlinked = 0 }) | ConvertTo-Json -Compress)
            }
            else {
                Write-Host ("[cache] no ledger found for worktree: " + $targetCanon)
            }
            exit 0
        }

        $liveSet = Get-LiveWorktreeSet -PrimaryRoot $primaryRoot
        if (-not $liveSet) {
            Write-Host "ERROR: could not enumerate live git worktrees; refusing to trim." -ForegroundColor Red
            exit 4
        }

        $handover = Read-HandoverState -CacheRoot $cacheDir
        $liveKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($ledger in $allLedgers) {
            if ($ledger.WorktreePath -ieq $targetCanon) { continue }
            if (-not $liveSet.Contains($ledger.WorktreePath)) { continue }
            foreach ($sig in $ledger.ActiveTargets.Values) {
                [void]$liveKeys.Add($sig)
            }
        }
        foreach ($sig in $handover.ActivePins.Keys) {
            [void]$liveKeys.Add($sig)
        }

        $processed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $deletedCount = 0
        [long]$freedBytes = 0
        $skippedHardlinked = 0
        $handoverPinned = 0
        $handoverChanged = $handover.Changed
        $expiryIso = [datetime]::UtcNow.AddHours([Math]::Max($HandoverHours, 1)).ToString('o')

        foreach ($ledger in $targetLedgers) {
            $activeSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
            foreach ($sig in $ledger.ActiveTargets.Values) { [void]$activeSet.Add($sig) }
            $pushedSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
            foreach ($sig in $ledger.PushedKeys) { [void]$pushedSet.Add($sig) }

            $isLanded = (-not $ledger.IsDirty) -and (Test-IsCommitLandedInPrimary -PrimaryRoot $primaryRoot -Commit $ledger.HeadCommit)
            $deleteCandidates = New-Object System.Collections.Generic.List[string]

            if ($isLanded) {
                foreach ($sig in $activeSet) {
                    if (-not $liveKeys.Contains($sig)) {
                        $handover.ActivePins[$sig] = [pscustomobject]@{
                            expires_at      = $expiryIso
                            source_worktree = $ledger.WorktreePath
                            commit          = $ledger.HeadCommit
                        }
                        [void]$liveKeys.Add($sig)
                        $handoverPinned++
                        $handoverChanged = $true
                    }
                }
                foreach ($sig in $pushedSet) {
                    if (-not $activeSet.Contains($sig) -and -not $liveKeys.Contains($sig)) {
                        $deleteCandidates.Add($sig)
                    }
                }
            }
            else {
                foreach ($sig in $activeSet) {
                    if (-not $liveKeys.Contains($sig)) { $deleteCandidates.Add($sig) }
                }
                foreach ($sig in $pushedSet) {
                    if (-not $liveKeys.Contains($sig)) { $deleteCandidates.Add($sig) }
                }
            }

            $remainingSkipped = New-Object System.Collections.Generic.List[string]
            foreach ($sig in $deleteCandidates) {
                $beforeSkipped = $skippedHardlinked
                $removed = Invoke-DeleteExclusiveKey -CacheRoot $cacheDir -Sig $sig -ProcessedKeys $processed -DeletedCount ([ref]$deletedCount) -FreedBytes ([ref]$freedBytes) -SkippedHardlinked ([ref]$skippedHardlinked)
                if (-not $removed -and $skippedHardlinked -gt $beforeSkipped) {
                    $remainingSkipped.Add($sig)
                }
            }

            if ($remainingSkipped.Count -gt 0) {
                if ($PSCmdlet.ShouldProcess($ledger.FilePath, 'Retain skipped hardlinked keys in dead worktree ledger')) {
                    $ledger.Raw.active_targets = @{}
                    $ledger.Raw.pushed_keys = @($remainingSkipped.ToArray())
                    $ledger.Raw.is_dirty = $true
                    Save-JsonFileUtf8 -Path $ledger.FilePath -Object $ledger.Raw
                }
            }
            elseif ($PSCmdlet.ShouldProcess($ledger.FilePath, 'Delete removed worktree cache ledger')) {
                try {
                    [System.IO.File]::Delete($ledger.FilePath)
                }
                catch {
                    Write-Verbose ("could not delete ledger " + $ledger.FilePath + ": " + $_.Exception.Message)
                }
            }
        }

        if ($handoverChanged) {
            Save-HandoverState -HandoverPath $handover.Path -Pins $handover.ActivePins
        }

        $after = Measure-CacheDirectory -Path $cacheDir
        if ($Json) {
            $payload = [pscustomobject]@{
                action             = $Action
                path               = $cacheDir
                worktree           = $targetCanon
                matched_ledgers    = $targetLedgers.Count
                deleted            = $deletedCount
                freed_mib          = [math]::Round($freedBytes / 1MB, 1)
                handover_pinned    = $handoverPinned
                skipped_hardlinked = $skippedHardlinked
                files_after        = $after.Files
                mib_after          = $after.MiB
            }
            Write-Output ($payload | ConvertTo-Json -Compress)
        }
        else {
            Write-Host ("[cache] trimmed worktree '" + (Split-Path -Leaf $targetCanon) + "': deleted " + $deletedCount + " entr(ies), freed " + [math]::Round($freedBytes / 1MB, 1) + " MiB, handover-pinned " + $handoverPinned + ", skipped-hardlinked " + $skippedHardlinked + "; now " + $after.MiB + " MiB in " + $after.Files + " files")
        }
        exit 0
    }

    'TrimOrphans' {
        if (-not $Force) {
            $live = @(Wait-ForIdleBuild -Seconds $WaitSeconds)
            if ($live.Count -gt 0) {
                if ($QuietIfBusy) {
                    if (-not $Json) {
                        Write-Host "[cache] TrimOrphans skipped (build active)." -ForegroundColor DarkGray
                    }
                    else {
                        Write-Output (([pscustomobject]@{ action = $Action; path = $cacheDir; skipped_busy = $true }) | ConvertTo-Json -Compress)
                    }
                    exit 0
                }
                if (-not $Json) {
                    Write-Host ("[cache] REFUSED: " + $live.Count + " build process(es) are running:") -ForegroundColor Red
                    foreach ($proc in $live) { Write-Host ("[cache]   PID " + $proc.ProcessId + " " + $proc.Name + " :: " + $proc.CommandLine) }
                }
                exit 3
            }
        }

        $liveSet = Get-LiveWorktreeSet -PrimaryRoot $primaryRoot
        if (-not $liveSet) {
            Write-Host "ERROR: could not enumerate live git worktrees; refusing to trim orphans." -ForegroundColor Red
            exit 4
        }

        $allLedgers = @(Get-WorktreeLedger -CacheRoot $cacheDir)
        $liveLedgers = New-Object System.Collections.Generic.List[object]
        $deadLedgers = New-Object System.Collections.Generic.List[object]
        foreach ($ledger in $allLedgers) {
            if ($ledger.WorktreePath -and $liveSet.Contains($ledger.WorktreePath)) {
                $liveLedgers.Add($ledger)
            }
            else {
                $deadLedgers.Add($ledger)
            }
        }

        $handover = Read-HandoverState -CacheRoot $cacheDir
        $handoverChanged = $handover.Changed

        # Remove handover pins that primary already materialized in its active_targets.
        $primaryActive = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($ledger in $liveLedgers) {
            if ($ledger.IsPrimary) {
                foreach ($sig in $ledger.ActiveTargets.Values) { [void]$primaryActive.Add($sig) }
            }
        }
        foreach ($sig in @($handover.ActivePins.Keys)) {
            if ($primaryActive.Contains($sig)) {
                $handover.ActivePins.Remove($sig)
                $handoverChanged = $true
            }
        }

        $liveKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($ledger in $liveLedgers) {
            foreach ($sig in $ledger.ActiveTargets.Values) {
                [void]$liveKeys.Add($sig)
            }
        }
        foreach ($sig in $handover.ActivePins.Keys) {
            [void]$liveKeys.Add($sig)
        }

        $processed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $deletedCount = 0
        $untrackedDeleted = 0
        [long]$freedBytes = 0
        $skippedHardlinked = 0
        $handoverPinned = 0
        $expiryIso = [datetime]::UtcNow.AddHours([Math]::Max($HandoverHours, 1)).ToString('o')

        foreach ($ledger in $deadLedgers) {
            $activeSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
            foreach ($sig in $ledger.ActiveTargets.Values) { [void]$activeSet.Add($sig) }
            $pushedSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
            foreach ($sig in $ledger.PushedKeys) { [void]$pushedSet.Add($sig) }

            $isLanded = (-not $ledger.IsDirty) -and (Test-IsCommitLandedInPrimary -PrimaryRoot $primaryRoot -Commit $ledger.HeadCommit)
            $deleteCandidates = New-Object System.Collections.Generic.List[string]

            if ($isLanded) {
                foreach ($sig in $activeSet) {
                    if (-not $liveKeys.Contains($sig)) {
                        $handover.ActivePins[$sig] = [pscustomobject]@{
                            expires_at      = $expiryIso
                            source_worktree = $ledger.WorktreePath
                            commit          = $ledger.HeadCommit
                        }
                        [void]$liveKeys.Add($sig)
                        $handoverPinned++
                        $handoverChanged = $true
                    }
                }
                foreach ($sig in $pushedSet) {
                    if (-not $activeSet.Contains($sig) -and -not $liveKeys.Contains($sig)) {
                        $deleteCandidates.Add($sig)
                    }
                }
            }
            else {
                foreach ($sig in $activeSet) {
                    if (-not $liveKeys.Contains($sig)) { $deleteCandidates.Add($sig) }
                }
                foreach ($sig in $pushedSet) {
                    if (-not $liveKeys.Contains($sig)) { $deleteCandidates.Add($sig) }
                }
            }

            $remainingSkipped = New-Object System.Collections.Generic.List[string]
            foreach ($sig in $deleteCandidates) {
                $beforeSkipped = $skippedHardlinked
                $removed = Invoke-DeleteExclusiveKey -CacheRoot $cacheDir -Sig $sig -ProcessedKeys $processed -DeletedCount ([ref]$deletedCount) -FreedBytes ([ref]$freedBytes) -SkippedHardlinked ([ref]$skippedHardlinked)
                if (-not $removed -and $skippedHardlinked -gt $beforeSkipped) {
                    $remainingSkipped.Add($sig)
                }
            }

            if ($remainingSkipped.Count -gt 0) {
                if ($PSCmdlet.ShouldProcess($ledger.FilePath, 'Retain skipped hardlinked keys in dead worktree ledger')) {
                    $ledger.Raw.active_targets = @{}
                    $ledger.Raw.pushed_keys = @($remainingSkipped.ToArray())
                    $ledger.Raw.is_dirty = $true
                    Save-JsonFileUtf8 -Path $ledger.FilePath -Object $ledger.Raw
                }
            }
            elseif ($PSCmdlet.ShouldProcess($ledger.FilePath, 'Delete dead worktree cache ledger')) {
                try {
                    [System.IO.File]::Delete($ledger.FilePath)
                }
                catch {
                    Write-Verbose ("could not delete ledger " + $ledger.FilePath + ": " + $_.Exception.Message)
                }
            }
        }

        # Sweep superseded intermediate keys from live worktree ledgers.
        foreach ($ledger in $liveLedgers) {
            $keptPushed = New-Object System.Collections.Generic.List[string]
            $ledgerChanged = $false
            foreach ($sig in $ledger.PushedKeys) {
                if ($liveKeys.Contains($sig)) {
                    $keptPushed.Add($sig)
                    continue
                }
                $removed = Invoke-DeleteExclusiveKey -CacheRoot $cacheDir -Sig $sig -ProcessedKeys $processed -DeletedCount ([ref]$deletedCount) -FreedBytes ([ref]$freedBytes) -SkippedHardlinked ([ref]$skippedHardlinked)
                if ($removed) {
                    $ledgerChanged = $true
                }
                else {
                    $keptPushed.Add($sig)
                }
            }
            if ($ledgerChanged -and $PSCmdlet.ShouldProcess($ledger.FilePath, 'Prune superseded keys from live ledger')) {
                $ledger.Raw.pushed_keys = @($keptPushed.ToArray())
                Save-JsonFileUtf8 -Path $ledger.FilePath -Object $ledger.Raw
            }
        }

        # Sweep expired handover pins.
        foreach ($sig in $handover.ExpiredKeys) {
            if (-not $liveKeys.Contains($sig)) {
                [void](Invoke-DeleteExclusiveKey -CacheRoot $cacheDir -Sig $sig -ProcessedKeys $processed -DeletedCount ([ref]$deletedCount) -FreedBytes ([ref]$freedBytes) -SkippedHardlinked ([ref]$skippedHardlinked))
            }
        }

        # Sweep untracked unlinked payload entries (e.g. legacy pre-ledger builds).
        if ($liveLedgers.Count -gt 0 -or $handover.ActivePins.Count -gt 0) {
            foreach ($entry in @(Get-CachePayloadFile -CacheRoot $cacheDir)) {
                $sig = $entry.Name
                if ($liveKeys.Contains($sig)) { continue }
                $beforeDeleted = $deletedCount
                [void](Invoke-DeleteExclusiveKey -CacheRoot $cacheDir -Sig $sig -ProcessedKeys $processed -DeletedCount ([ref]$deletedCount) -FreedBytes ([ref]$freedBytes) -SkippedHardlinked ([ref]$skippedHardlinked))
                if ($deletedCount -gt $beforeDeleted) {
                    $untrackedDeleted++
                }
            }
        }

        if ($handoverChanged) {
            Save-HandoverState -HandoverPath $handover.Path -Pins $handover.ActivePins
        }

        # Optional LRU cap enforcement (unlinked entries first, then oldest overall).
        if ($MaxMiB -gt 0) {
            $afterSweep = Measure-CacheDirectory -Path $cacheDir
            $targetBytes = [long]$MaxMiB * 1MB
            if ($afterSweep.Bytes -gt $targetBytes) {
                [long]$remaining = $afterSweep.Bytes
                $ordered = @(Get-CachePayloadFile -CacheRoot $cacheDir | Sort-Object LastWriteTime)
                foreach ($pass in @(1, 2)) {
                    foreach ($entry in $ordered) {
                        if ($remaining -le $targetBytes) { break }
                        if (-not (Test-Path -LiteralPath $entry.FullName -PathType Leaf)) { continue }
                        if ($pass -eq 1 -and (Get-FileHardLinkCount -Path $entry.FullName) -gt 1) { continue }
                        if ($PSCmdlet.ShouldProcess($entry.FullName, 'Prune cache entry to fit MaxMiB')) {
                            try {
                                $length = $entry.Length
                                [System.IO.File]::Delete($entry.FullName)
                                $remaining -= $length
                                $freedBytes += $length
                                $deletedCount++
                            }
                            catch {
                                Write-Verbose ("could not delete " + $entry.FullName + ": " + $_.Exception.Message)
                            }
                        }
                    }
                }
            }
        }

        $after = Measure-CacheDirectory -Path $cacheDir
        if ($Json) {
            $payload = [pscustomobject]@{
                action             = $Action
                path               = $cacheDir
                dead_ledgers       = $deadLedgers.Count
                live_ledgers       = $liveLedgers.Count
                deleted            = $deletedCount
                untracked_deleted  = $untrackedDeleted
                freed_mib          = [math]::Round($freedBytes / 1MB, 1)
                handover_pinned    = $handoverPinned
                skipped_hardlinked = $skippedHardlinked
                files_after        = $after.Files
                mib_after          = $after.MiB
            }
            Write-Output ($payload | ConvertTo-Json -Compress)
        }
        else {
            $untrackedNote = if ($untrackedDeleted -gt 0) { " (incl. " + $untrackedDeleted + " untracked)" } else { "" }
            Write-Host ("[cache] TrimOrphans: " + $deadLedgers.Count + " dead ledger(s), " + $liveLedgers.Count + " live ledger(s); deleted " + $deletedCount + " entr(ies)" + $untrackedNote + ", freed " + [math]::Round($freedBytes / 1MB, 1) + " MiB, handover-pinned " + $handoverPinned + ", skipped-hardlinked " + $skippedHardlinked + "; now " + $after.MiB + " MiB in " + $after.Files + " files")
        }
        exit 0
    }
}
