# Don't use verbose unless an error happened and it requires inspecting
# every build process' output.

param(
    [string]$Target = "template_debug",
    [switch]$IncludePdb,
    [switch]$IncludeTests,
    [switch]$Verbose,
    [switch]$SkipVerify,
    [switch]$Local,
    [switch]$Remote,
    [string]$EncryptionKey = "",
    [string]$EncryptionKeyFile = "",
    [switch]$ResetKeyHeader,
    [switch]$KeepRemoteKeyHeader,
    [switch]$KeepLocalKeyHeader,
    [switch]$RequireManifest,
    [switch]$AuditOnly,
    [string[]]$AuditPath = @(),
    [switch]$Json
)

$ScriptStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Get-FormattedElapsed {
    param([System.Diagnostics.Stopwatch]$Stopwatch)
    if (-not $Stopwatch) { return "" }
    $elapsed = $Stopwatch.Elapsed
    if ($elapsed.TotalMinutes -ge 1) {
        $mins = [int][Math]::Floor($elapsed.TotalMinutes)
        return ("{0}m {1:D2}s ({2:F1}s total)" -f $mins, $elapsed.Seconds, $elapsed.TotalSeconds)
    }
    return ("{0:F1}s" -f $elapsed.TotalSeconds)
}

# Snapshot the genuine process environment BEFORE .env is merged into it (R9-02).
# The Godot export plugin reads the OS environment only, so a key declared in .env is not the
# R7-14 hazard and must not abort compiling builds.
$AmbientKeySnapshot = $env:DSPKG_ENCRYPTION_KEY

function Write-Status {
    param(
        [string]$Message = "",
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray,
        [ConsoleColor]$BackgroundColor
    )
    if ($Json) {
        [Console]::Error.WriteLine($Message)
    }
    else {
        if ($PSBoundParameters.ContainsKey('BackgroundColor')) {
            Write-Host $Message -ForegroundColor $ForegroundColor -BackgroundColor $BackgroundColor
        }
        else {
            Write-Host $Message -ForegroundColor $ForegroundColor
        }
    }
}

function Show-BuildFailure {
    param(
        [string]$Step,
        [string]$Message,
        [int]$Code = 1
    )
    $elapsedStr = Get-FormattedElapsed $ScriptStopwatch
    Write-Status ""
    Write-Status "================================================================================" -ForegroundColor Red
    Write-Status " [BUILD FAILED] $Step " -ForegroundColor White -BackgroundColor DarkRed
    Write-Status "================================================================================" -ForegroundColor Red
    Write-Status " Reason: $Message" -ForegroundColor Red
    if ($Code -ne 0) {
        Write-Status " Exit Code: $Code" -ForegroundColor Yellow
    }
    if ($elapsedStr) {
        Write-Status " Elapsed Time: $elapsedStr" -ForegroundColor DarkGray
    }
    Write-Status " Check the compiler/terminal output above for details." -ForegroundColor DarkGray
    Write-Status "================================================================================" -ForegroundColor Red
    Write-Status ""
    exit $Code
}

function Show-BuildSuccess {
    param(
        [string]$Message = "All steps completed successfully!"
    )
    $elapsedStr = Get-FormattedElapsed $ScriptStopwatch
    Write-Status ""
    Write-Status "================================================================================" -ForegroundColor Green
    Write-Status " [BUILD SUCCESS] $Message " -ForegroundColor White -BackgroundColor DarkGreen
    if ($elapsedStr) {
        Write-Status " Duration: $elapsedStr" -ForegroundColor Green
    }
    Write-Status "================================================================================" -ForegroundColor Green
    Write-Status ""
}

function Write-VerboseHost {
    param(
        [string]$Message,
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray
    )
    if ($Verbose) {
        Write-Status $Message -ForegroundColor $ForegroundColor
    }
}

# --- Deployed-binary audit (read-only, advisory) -----------------------------
# Compares the files actually present in a deployed addon directory against the
# paths declared by that directory's .gdextension (the only files Godot loads).
# It never deletes, moves, or repairs anything: binaries left behind by an older
# naming scheme, hand copies, or stray experiments are reported so a human or an
# agent can decide what to remove.
function Get-DeployBinAudit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$AddonDir,
        [switch]$RequireManifest
    )

    if (-not (Test-Path -LiteralPath $AddonDir -PathType Container)) {
        $skipStatus = if ($RequireManifest) { 'error' } else { 'skipped' }
        return [pscustomobject]@{ addon_dir = $AddonDir; status = $skipStatus; reason = 'addon directory not found'; expected = @(); missing = @(); undeclared = @(); ignored_foreign = 0; binaries = @() }
    }

    $manifestFiles = @(Get-ChildItem -LiteralPath $AddonDir -Filter '*.gdextension' -File -ErrorAction SilentlyContinue)
    if ($manifestFiles.Count -eq 0) {
        $skipStatus = if ($RequireManifest) { 'error' } else { 'skipped' }
        return [pscustomobject]@{ addon_dir = $AddonDir; status = $skipStatus; reason = 'no .gdextension manifest found'; expected = @(); missing = @(); undeclared = @(); ignored_foreign = 0; binaries = @() }
    }

    # Only [libraries] and [dependencies] declare files that must exist on disk.
    # Everything else in the file (comments, entry_symbol, ...) is ignored.
    # Each declaration key starts with a platform tag (windows./linux./macos.),
    # which lets us ignore foreign-platform entries that cannot exist here.
    $expectedEntries = @()
    foreach ($manifest in $manifestFiles) {
        $section = ''
        foreach ($rawLine in @(Get-Content -LiteralPath $manifest.FullName -ErrorAction SilentlyContinue)) {
            $line = $rawLine.Trim()
            if ($line -match '^\[(?<name>.+)\]$') {
                $section = $matches['name'].ToLowerInvariant()
                continue
            }
            if ($line.StartsWith(';') -or $line.StartsWith('#')) { continue }
            if ($section -ne 'libraries' -and $section -ne 'dependencies') { continue }

            $declared = $line
            $platform = ''
            if ($line -match '^(?<key>[A-Za-z0-9_.]+)\s*=\s*(?<rest>.*)$') {
                $platform = ($matches['key'] -split '\.')[0].ToLowerInvariant()
                $declared = $matches['rest']
            }

            $pattern = '"(?<path>[^"]*\.(?:dll|so|dylib))"'
            foreach ($match in [regex]::Matches($declared, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
                $rel = $match.Groups['path'].Value.Trim()
                $rel = $rel.Replace('/', '\')
                if ($rel.StartsWith('.\')) { $rel = $rel.Substring(2) }
                if (-not $rel) { continue }
                if (@($expectedEntries | Where-Object { $_.rel -eq $rel }).Count -gt 0) { continue }
                $expectedEntries += [pscustomobject]@{ rel = $rel; platform = $platform; section = $section }
            }
        }
    }

    if ($expectedEntries.Count -eq 0) {
        $skipStatus = if ($RequireManifest) { 'error' } else { 'skipped' }
        return [pscustomobject]@{ addon_dir = $AddonDir; status = $skipStatus; reason = 'manifest declares no shared libraries'; expected = @(); missing = @(); undeclared = @(); ignored_foreign = 0; binaries = @() }
    }

    $expectedRel = @($expectedEntries | ForEach-Object { $_.rel })

    # Foreign-platform libraries are declared but intentionally absent on this OS.
    $currentPlatform = 'windows'
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        if ($IsLinux) { $currentPlatform = 'linux' }
        elseif ($IsMacOS) { $currentPlatform = 'macos' }
    }

    $expectedFull = @()
    foreach ($rel in $expectedRel) {
        $expectedFull += [System.IO.Path]::GetFullPath((Join-Path $AddonDir $rel))
    }

    # Audit only the directories that actually hold declared files, so unrelated
    # subfolders (icons/, doc_classes/, scripts/, ...) are never reported.
    $auditDirs = @()
    foreach ($full in $expectedFull) {
        $parent = [System.IO.Path]::GetDirectoryName($full)
        if ($parent -and ($auditDirs -notcontains $parent) -and (Test-Path -LiteralPath $parent -PathType Container)) {
            $auditDirs += $parent
        }
    }
    if ($auditDirs.Count -eq 0) {
        $fallbackDir = Join-Path $AddonDir 'bin'
        if (Test-Path -LiteralPath $fallbackDir -PathType Container) {
            $auditDirs += [System.IO.Path]::GetFullPath($fallbackDir)
        }
    }

    # Files that legitimately live beside the binaries without being declared.
    $allowNames = @('build_manifest.json', '.gitkeep', '.gdignore', 'desktop.ini', 'Thumbs.db')

    $missing = @()
    $ignoredForeign = 0
    foreach ($entry in $expectedEntries) {
        if ($entry.platform -and $entry.platform -ne $currentPlatform) {
            $ignoredForeign += 1
            continue
        }
        $full = [System.IO.Path]::GetFullPath((Join-Path $AddonDir $entry.rel))
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            $name = [System.IO.Path]::GetFileName($full)
            if ($missing -notcontains $name) { $missing += $name }
        }
    }

    $expectedSet = @{}
    foreach ($full in $expectedFull) { $expectedSet[$full.ToLowerInvariant()] = $true }

    $undeclared = @()
    foreach ($dir in $auditDirs) {
        foreach ($file in @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue)) {
            if ($expectedSet.ContainsKey($file.FullName.ToLowerInvariant())) { continue }
            if ($allowNames -contains $file.Name) { continue }
            # Symbol files accompany a DLL; .old files are transient swap artifacts.
            if ($file.Name.EndsWith('.pdb', [System.StringComparison]::OrdinalIgnoreCase)) { continue }
            if ($file.Name.EndsWith('.old', [System.StringComparison]::OrdinalIgnoreCase)) { continue }
            $undeclared += [pscustomobject]@{
                name          = $file.Name
                path          = $file.FullName
                size_bytes    = $file.Length
                last_modified = $file.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
            }
        }
    }

    $buildManifestJson = $null
    $manifestPath = Join-Path $AddonDir "bin\build_manifest.json"
    if (Test-Path -LiteralPath $manifestPath) {
        try {
            $buildManifestJson = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        } catch {}
    }

    $binAudits = @()
    foreach ($entry in $expectedEntries) {
        if ($entry.platform -and $entry.platform -ne $currentPlatform) { continue }
        $full = [System.IO.Path]::GetFullPath((Join-Path $AddonDir $entry.rel))
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }

        $targetFileName = [System.IO.Path]::GetFileName($entry.rel)
        if ($entry.section -eq 'libraries') {
            $binaryTag = "<missing>"
            try {
                $rawAscii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($full))
                if ($rawAscii -match 'dspkg-key-id=(?<id>[0-9a-fA-F]{16}|none)') {
                    $binaryTag = $matches['id'].ToLowerInvariant()
                }
            } catch {}

            $targetProp = if ($buildManifestJson -and $buildManifestJson.targets) { $buildManifestJson.targets.PSObject.Properties[$targetFileName] } else { $null }
            $manifestEnc = if ($targetProp) { $targetProp.Value.encryption } else { $null }
            $manifestKeyId = if ($manifestEnc -and $manifestEnc.enabled -and $manifestEnc.key_id) {
                $manifestEnc.key_id.ToLowerInvariant()
            } else {
                "none"
            }
            $isMatch = ($binaryTag -eq $manifestKeyId)

            $binAudits += [pscustomobject]@{
                target          = $targetFileName
                path            = $full
                section         = 'libraries'
                tag             = $binaryTag
                manifest_key_id = $manifestKeyId
                isMatch         = $isMatch
                verified        = $true
            }
        }
        else {
            $binAudits += [pscustomobject]@{
                target          = $targetFileName
                path            = $full
                section         = 'dependencies'
                tag             = "not_applicable"
                manifest_key_id = "not_applicable"
                isMatch         = $true
                verified        = $true
            }
        }
    }

    $audit = [pscustomobject]@{
        addon_dir       = $AddonDir
        status          = 'clean'
        reason          = ''
        expected        = $expectedRel
        missing         = $missing
        undeclared      = $undeclared
        ignored_foreign = $ignoredForeign
        binaries        = $binAudits
    }

    if (-not $RequireManifest -and @($binAudits | Where-Object { $_.tag -match '^[0-9a-fA-F]{16}$' }).Count -gt 0) {
        # A keyed binary implies a manifest is expected (R8-13)
        $RequireManifest = $true
    }

    $manifestMissing = ($null -eq $buildManifestJson)
    $mismatchCount = @($binAudits | Where-Object { $_.isMatch -eq $false }).Count
    if ($manifestMissing -and $RequireManifest) {
        $audit.status = 'error'
        $audit.reason = 'build_manifest.json missing from addon bin directory (required for keyed/release build)'
    }
    elseif ($manifestMissing) {
        $audit.status = 'skipped'
        $audit.reason = 'build_manifest.json not present in addon bin directory'
    }
    elseif ($missing.Count -gt 0) {
        $audit.status = 'error'
    }
    elseif ($mismatchCount -gt 0) {
        $audit.status = 'mismatch'
    }
    elseif ($undeclared.Count -gt 0) {
        $audit.status = 'findings'
    }
    else {
        $audit.status = 'clean'
    }

    return $audit
}

function Write-DeployBinAudit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Audit
    )

    if ($Audit.status -eq 'skipped') {
        # Visible by default: a target that resolves to nothing (wrong subpath,
        # missing directory) must not look like a clean result.
        Write-Host ("[audit] Skipped {0} ({1})" -f $Audit.addon_dir, $Audit.reason) -ForegroundColor DarkGray
        return
    }

    if ($Audit.status -eq 'error') {
        Write-Host ("[audit] {0}: ERROR" -f $Audit.addon_dir) -ForegroundColor Red
        if ($Audit.reason) {
            Write-Host ("  {0}" -f $Audit.reason) -ForegroundColor Red
        }
        if ($Audit.missing.Count -gt 0) {
            Write-Host ("  MISSING ({0}) - declared in .gdextension but absent:" -f $Audit.missing.Count) -ForegroundColor Red
            foreach ($name in $Audit.missing) {
                Write-Host ("    {0}" -f $name) -ForegroundColor Red
            }
        }
        return
    }

    if ($Audit.status -eq 'mismatch') {
        Write-Host ("[audit] {0}: MISMATCH - encryption key-id tag does not match build manifest" -f $Audit.addon_dir) -ForegroundColor Red
        foreach ($bin in @($Audit.binaries | Where-Object { $_.isMatch -eq $false })) {
            Write-Host ("  {0}: binary tag '{1}' != manifest key '{2}'" -f $bin.target, $bin.tag, $bin.manifest_key_id) -ForegroundColor Red
        }
        return
    }

    $binSummaries = @($Audit.binaries | Where-Object { $_.tag -ne "not_applicable" } | ForEach-Object {
        $statusText = if ($_.tag -eq "none") { "keyless" } elseif ($_.tag -eq "<missing>") { "unversioned" } else { "keyed ($($_.tag))" }
        "{0}: {1}" -f $_.target, $statusText
    }) -join ", "

    if ($Audit.status -eq 'clean') {
        Write-Host ("[audit] {0}: OK - {1} declared file(s), nothing unexpected" -f $Audit.addon_dir, $Audit.expected.Count) -ForegroundColor Green
        if ($binSummaries) {
            Write-Host ("  [{0}]" -f $binSummaries) -ForegroundColor DarkGray
        }
        if ($Audit.ignored_foreign -gt 0) {
            Write-Host ("  ({0} foreign-platform library entry/entries ignored)" -f $Audit.ignored_foreign) -ForegroundColor DarkGray
        }
        return
    }

    Write-Host ("[audit] {0}" -f $Audit.addon_dir) -ForegroundColor Cyan
    if ($Audit.undeclared.Count -gt 0) {
        Write-Host ("  UNDECLARED ({0}) - on disk but not referenced by .gdextension:" -f $Audit.undeclared.Count) -ForegroundColor Yellow
        foreach ($item in $Audit.undeclared) {
            Write-Host ("    {0}  {1} bytes  {2}" -f $item.name, $item.size_bytes, $item.last_modified) -ForegroundColor Yellow
        }
        Write-Host "  Advisory only - nothing was deleted or moved." -ForegroundColor DarkGray
    }
    if ($binSummaries) {
        Write-Host ("  [{0}]" -f $binSummaries) -ForegroundColor DarkGray
    }
    if ($Audit.ignored_foreign -gt 0) {
        Write-Host ("  ({0} foreign-platform library entry/entries ignored)" -f $Audit.ignored_foreign) -ForegroundColor DarkGray
    }
}

# Deployment destinations are entirely environment-driven (see .env.example and
# SConstruct). Each variable names a different root shape, so the addon
# subdirectory differs per target:
#   SYNC_ADDON_DIR          -> the addon directory itself
#   SYNC_ADDON_COPYABLE_DIR -> <base>\directstorage_accelerator  (+ <base>\src)
#   SYNC_TEST_DIR           -> mirror of project/ -> <base>\addons\directstorage_accelerator
# Nothing here is hardcoded to a specific checkout.
function Get-ConfiguredAddonDir {
    [CmdletBinding()]
    param()

    $dirs = @()
    if (-not [string]::IsNullOrWhiteSpace($env:SYNC_ADDON_DIR)) {
        $dirs += $env:SYNC_ADDON_DIR
    }
    if (-not [string]::IsNullOrWhiteSpace($env:SYNC_ADDON_COPYABLE_DIR)) {
        $dirs += (Join-Path $env:SYNC_ADDON_COPYABLE_DIR 'directstorage_accelerator')
    }
    if (-not [string]::IsNullOrWhiteSpace($env:SYNC_TEST_DIR)) {
        $dirs += (Join-Path $env:SYNC_TEST_DIR 'addons\directstorage_accelerator')
    }
    return $dirs
}

function Invoke-DeployAudit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AddonDirs,
        [switch]$Json,
        [switch]$RequireManifest
    )

    $results = @()
    $seen = @()
    foreach ($dir in $AddonDirs) {
        if ([string]::IsNullOrWhiteSpace($dir)) { continue }
        $resolved = $dir
        if (Test-Path -LiteralPath $dir) {
            $resolved = (Resolve-Path -LiteralPath $dir).Path
        }
        $dedupeKey = $resolved.ToLowerInvariant()
        if ($seen -contains $dedupeKey) { continue }
        $seen += $dedupeKey
        $results += Get-DeployBinAudit -AddonDir $resolved -RequireManifest:$RequireManifest
    }

    $hasError = @($results | Where-Object { $_.status -in @('mismatch', 'error') }).Count -gt 0

    if ($Json) {
        [Console]::Out.WriteLine((ConvertTo-Json -InputObject @($results) -Depth 5))
    }
    else {
        foreach ($audit in $results) {
            Write-DeployBinAudit -Audit $audit
        }
    }

    return $hasError
}

# Load configuration from .env file
$EnvFileCandidates = @(
    (Join-Path $PSScriptRoot "..\.env"),
    (Join-Path $PWD ".env"),
    (Join-Path $PSScriptRoot "build_remote.env")
)

$EnvFilePath = $null
foreach ($path in $EnvFileCandidates) {
    if (Test-Path $path) {
        $EnvFilePath = (Resolve-Path $path).Path
        break
    }
}

if (-not $EnvFilePath) {
    Show-BuildFailure -Step "Configuration Error" -Message "Configuration file '.env' not found! Please copy '.env.example' to '.env' and configure REMOTE_HOST and REMOTE_DIR." -Code 1
}

# Parse .env file (preserve pre-existing process environment variables to enforce ambient > .env precedence, R8-06)
Get-Content $EnvFilePath | ForEach-Object {
    $line = $_.Trim()
    if ($line -and -not $line.StartsWith("#")) {
        $key, $value = $line -split '=', 2
        if ($key -and $value) {
            $k = $key.Trim()
            if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($k, "Process"))) {
                [Environment]::SetEnvironmentVariable($k, $value.Trim().Trim('"').Trim("'"), "Process")
            }
        }
    }
}

# Local-first pipeline mode: default to local SCons unless REMOTE_BUILD=1
# (or -Remote is passed). -Local always forces local mode.
$IsRemote = ($Remote) -or (-not $Local -and ($env:REMOTE_BUILD -in @("1", "true", "yes")))

$WorkspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$IsMultiTarget = ($Target -in @("template_debug_and_release", "all"))
$TestsFlag = if ($IncludeTests) { " tests=true" } else { "" }
$DefaultAddonDir = Join-Path $WorkspaceRoot "project\addons\directstorage_accelerator"

function New-DspkgKeyHeader {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [string]$KeyHex = "",
        [switch]$Reset
    )
    $outPath = [System.IO.Path]::GetFullPath((Join-Path $WorkspaceRoot "src\gen\dspkg_encryption_key.h"))
    $parentDir = Split-Path -Path $outPath -Parent
    if (-not (Test-Path -LiteralPath $parentDir -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($parentDir) | Out-Null
    }

    if ($Reset -or -not $KeyHex) {
        $content = "// Auto-generated bootstrap fallback. Overwritten when key is provided.`n#pragma once`n`n#define DSPKG_ENCRYPTION_ENABLED 0`n#define DSPKG_KEY_ID_TAG `"dspkg-key-id=none`"`n"
    }
    else {
        $raw = [byte[]]::new(32)
        for ($i = 0; $i -lt 32; $i++) {
            $raw[$i] = [Convert]::ToByte($KeyHex.Substring($i * 2, 2), 16)
        }
        $sha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($raw)
        $mask = $sha[0..31]
        $masked = for ($i = 0; $i -lt 32; $i++) { [byte]($raw[$i] -bxor $mask[$i]) }

        # Pass secret exclusively via stdin anonymous pipe; command-line string is static
        # Filter hex characters to tolerate PowerShell pipeline encoding and UTF-8 BOM (\ufeff)
        $pyCode = "import sys,hashlib; txt = ''.join([c for c in sys.stdin.read() if c in '0123456789abcdefABCDEF']); assert len(txt) == 64; print(hashlib.blake2b(bytes.fromhex(txt), digest_size=8).hexdigest().lower())"

        # Discover Python interpreter without cmd.exe or WindowsApps stub
        $pyExe = $null
        $pyArgs = @()
        $pyCandidate = Get-Command python -ErrorAction SilentlyContinue
        if ($pyCandidate -and $pyCandidate.Source -notmatch 'WindowsApps') {
            $pyExe = $pyCandidate.Source
            $pyArgs = @("-c", $pyCode)
        }
        else {
            $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
            if ($pyLauncher) {
                $pyExe = $pyLauncher.Source
                $pyArgs = @("-3", "-c", $pyCode)
            }
        }
        if (-not $pyExe) {
            Show-BuildFailure -Step "Validation" -Message "Python 3 interpreter not found on PATH. Required for BLAKE2b key ID derivation." -Code 1
        }

        $keyId = ($KeyHex | & $pyExe @pyArgs 2>$null).Trim()
        if ($LASTEXITCODE -ne 0 -or $keyId -notmatch '^[0-9a-f]{16}$') {
            Show-BuildFailure -Step "KeyHeader" -Message "Failed to derive 16-hex key ID via Python stdin pipe." -Code 1
        }

        $maskStr = ($mask | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
        $maskedStr = ($masked | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "

        $content = "// Auto-generated by build pipeline. DO NOT COMMIT OR EDIT MANUALLY.`n#pragma once`n`n#define DSPKG_ENCRYPTION_ENABLED 1`n#define DSPKG_KEY_ID_TAG `"dspkg-key-id=$keyId`"`n`nnamespace dspkg_key {`n    constexpr unsigned char MASK[32] = {`n        $maskStr`n    };`n`n    constexpr unsigned char MASKED_KEY[32] = {`n        $maskedStr`n    };`n`n    static_assert(sizeof(MASK) == 32 && sizeof(MASKED_KEY) == 32, `"Mask and key arrays must be exactly 32 bytes`");`n} // namespace dspkg_key`n"
    }

    # Churn Guard with normalized LF and case-sensitive ordinal match
    if (Test-Path -LiteralPath $outPath -PathType Leaf) {
        $existingContent = [System.IO.File]::ReadAllText($outPath).Replace("`r`n", "`n")
        if ($existingContent -ceq $content) {
            return
        }
    }

    if ($PSCmdlet.ShouldProcess($outPath, "Write DSPKG encryption key header")) {
        $tmpPath = "$outPath.tmp"
        try {
            [System.IO.File]::WriteAllText($tmpPath, $content, [System.Text.UTF8Encoding]::new($false))
            if (Test-Path -LiteralPath $outPath) {
                [System.IO.File]::Replace($tmpPath, $outPath, [NullString]::Value)
            }
            else {
                [System.IO.File]::Move($tmpPath, $outPath)
            }
        }
        finally {
            if (Test-Path -LiteralPath $tmpPath) {
                Remove-Item -LiteralPath $tmpPath -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

if ($EncryptionKeyFile -and $EncryptionKey) {
    Show-BuildFailure -Step "Validation" -Message "Pass either -EncryptionKeyFile or -EncryptionKey, not both." -Code 1
}
if ($ResetKeyHeader -and ($EncryptionKeyFile -or $EncryptionKey)) {
    Show-BuildFailure -Step "Validation" -Message "-ResetKeyHeader cannot be combined with key input arguments." -Code 1
}

# Audit-only mode: report deployment contents without compiling or syncing.
if ($AuditOnly) {
    # Ensure fresh checkout has Branch B header even if SCons has not run, or reset header if -ResetKeyHeader (R7-03, R8-10)
    $genHeaderPath = Join-Path $WorkspaceRoot "src\gen\dspkg_encryption_key.h"
    if ($ResetKeyHeader -or -not (Test-Path -LiteralPath $genHeaderPath)) {
        New-DspkgKeyHeader -Reset
    }

    # Audit must not depend on key material: key resolution runs only after this block (R10-09), so the
    # manifest requirement comes from the explicit switch plus per-directory keyed-tag inference (R8-13).
    if ($EncryptionKeyFile -or $EncryptionKey) {
        Write-Status "[AuditOnly] Key arguments are ignored in audit mode; pass -RequireManifest to require build_manifest.json." -ForegroundColor Yellow
    }
    $AuditTargets = @()
    if ($AuditPath.Count -gt 0) {
        $AuditTargets = @($AuditPath)
    }
    else {
        $AuditTargets = @($DefaultAddonDir) + @(Get-ConfiguredAddonDir)
    }
    $auditExpectsManifest = [bool]$RequireManifest
    $hasError = Invoke-DeployAudit -AddonDirs $AuditTargets -Json:$Json -RequireManifest:$auditExpectsManifest
    if ($hasError) { exit 1 } else { exit 0 }
}

$ResolvedKeyFile = ""
$KeyFileRelWin = ""
$EffectiveKey = ""
if ($ResetKeyHeader) {
    # Strip ambient key variables from child build environments so child processes cannot re-key (R7-13)
    Remove-Item Env:DSPKG_ENCRYPTION_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:DSPKG_ENCRYPTION_KEY_FILE -ErrorAction SilentlyContinue
}
else {
    $CandidateKeyFile = $EncryptionKeyFile
    $isCliKeyFile = [bool]$CandidateKeyFile
    if (-not $CandidateKeyFile -and $env:DSPKG_ENCRYPTION_KEY_FILE) {
        $CandidateKeyFile = $env:DSPKG_ENCRYPTION_KEY_FILE
        $isCliKeyFile = $false
    }
    if ($CandidateKeyFile) {
        $ResolvedKeyFile = if ([System.IO.Path]::IsPathRooted($CandidateKeyFile)) {
            $CandidateKeyFile
        } elseif ($isCliKeyFile) {
            # CLI argument resolves against caller's current working directory (R7-11)
            [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $CandidateKeyFile))
        } else {
            # Ambient .env fallback resolves against repository root (R7-11)
            [System.IO.Path]::GetFullPath((Join-Path $WorkspaceRoot $CandidateKeyFile))
        }
        if (-not (Test-Path -LiteralPath $ResolvedKeyFile -PathType Leaf)) {
            Show-BuildFailure -Step "Validation" -Message "EncryptionKeyFile not found or is not a file: $ResolvedKeyFile" -Code 1
        }
        $fileItem = Get-Item -LiteralPath $ResolvedKeyFile
        if ($fileItem.Length -eq 0) {
            Show-BuildFailure -Step "Validation" -Message "EncryptionKeyFile is empty." -Code 1
        }
        # Strip whitespace and any leading/trailing UTF-8/UTF-16 BOM (R6-09)
        $rawFileContent = [System.IO.File]::ReadAllText($ResolvedKeyFile).Trim().Trim([char]0xFEFF)
        if ($rawFileContent -notmatch '^[0-9a-fA-F]{64}$') {
            Show-BuildFailure -Step "Validation" -Message "EncryptionKeyFile content must be a 64-character hexadecimal string." -Code 1
        }
        $EffectiveKey = $rawFileContent

        # Strip ambient key file variable when CLI key is provided to ensure SCons cannot invert precedence (R7-01)
        if ($isCliKeyFile) {
            Remove-Item Env:DSPKG_ENCRYPTION_KEY_FILE -ErrorAction SilentlyContinue
        }

        # For tar exclusion and remote cleanup
        $rootWithSep = $WorkspaceRoot.TrimEnd('\', '/') + '\'
        if ($ResolvedKeyFile.StartsWith($rootWithSep, [System.StringComparison]::OrdinalIgnoreCase)) {
            $KeyFileRelWin = $ResolvedKeyFile.Substring($rootWithSep.Length)
        }
    }
    elseif ($EncryptionKey) {
        Write-Status "[Security Warning] Passing -EncryptionKey exposes the raw key in process argument vectors, Event 4688, and shell history. Prefer using -EncryptionKeyFile." -ForegroundColor Yellow
        $trimmedKey = $EncryptionKey.Trim().Trim([char]0xFEFF)
        if ($trimmedKey -notmatch '^[0-9a-fA-F]{64}$') {
            Show-BuildFailure -Step "Validation" -Message "EncryptionKey must be a 64-character hexadecimal string." -Code 1
        }
        $EffectiveKey = $trimmedKey
        Remove-Item Env:DSPKG_ENCRYPTION_KEY_FILE -ErrorAction SilentlyContinue
    }
}

# Guard against a genuinely exported DSPKG_ENCRYPTION_KEY leaking into the export plugin without key arguments
# (R7-14, R8-01, R8-02). Evaluated strictly AFTER $CandidateKeyFile / $EffectiveKey resolution (R8-01), strictly for
# compiling builds (R8-02), and strictly on the pre-.env snapshot (R9-02) so a key declared only in .env - which the
# Godot editor never reads - does not abort the build.
if (-not $AuditOnly -and $AmbientKeySnapshot -and -not $EffectiveKey -and -not $ResetKeyHeader) {
    Show-BuildFailure -Step "Validation" -Message "DSPKG_ENCRYPTION_KEY is exported in the process environment, but builds require -EncryptionKeyFile, -EncryptionKey, or DSPKG_ENCRYPTION_KEY_FILE. Building a keyless DLL while the export plugin uses this exported key would cause runtime decryption failure. Unset DSPKG_ENCRYPTION_KEY (or move it to .env, where only build scripts read it), supply -EncryptionKeyFile, or pass -ResetKeyHeader." -Code 1
}

# A DSPKG_ENCRYPTION_KEY that came from .env (i.e. is not in the pre-.env snapshot) is invisible to the
# Godot editor and useless for the build, but would still be inherited by SCons and trip its own gate.
# Drop it from the child environment with a warning so both entry points agree (R9-02).
if ($env:DSPKG_ENCRYPTION_KEY -and -not $AmbientKeySnapshot) {
    Write-Status "[Config] Ignoring DSPKG_ENCRYPTION_KEY declared in .env: the Godot editor cannot read .env. Export it in your shell if the export plugin needs it." -ForegroundColor Yellow
    Remove-Item Env:DSPKG_ENCRYPTION_KEY -ErrorAction SilentlyContinue
}

if (-not $IsRemote) {
    # --- Local Mode (Default): compile directly with SCons ---
    Write-Status "[Local] Building target=$Target locally with SCons..." -ForegroundColor Cyan
    if ($IncludeTests -and -not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot "extern/gdextest/SConscript"))) {
        Write-Status "extern/gdextest submodule is missing; running 'git submodule update --init -- extern/gdextest'..." -ForegroundColor Yellow
        try {
            $subOut = & git submodule update --init -- extern/gdextest 2>&1
            if ($subOut -and $Verbose) {
                $subOut | ForEach-Object { Write-Status $_ }
            }
        }
        catch {
            $null = $_
        }
        if (-not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot "extern/gdextest/SConscript"))) {
            Show-BuildFailure -Step "Configuration Error" -Message "extern/gdextest is missing and auto-init failed. Run 'git submodule update --init -- extern/gdextest' manually (requires git and network access)." -Code 1
        }
    }
    $LocalVerifier = Join-Path $PSScriptRoot "verify_remote.ps1"
    if (Test-Path -LiteralPath $LocalVerifier) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $LocalVerifier -RemoteDir $WorkspaceRoot -CleanOrphans -ResetMarker 2>&1 | ForEach-Object {
            $cText = $_.ToString()
            if ($cText.StartsWith("KILLED_ORPHAN:")) {
                Write-Status "    Terminated orphaned build process: $($cText.Substring(14))" -ForegroundColor Yellow
            }
        }
    }
    Push-Location $WorkspaceRoot
    try {
        # Header write is inside the protected region so every exit path (including Show-BuildFailure)
        # runs the finally reset below; see the remote-mode rule in R9-05 (R10-06).
        $genHeaderPath = Join-Path $WorkspaceRoot "src\gen\dspkg_encryption_key.h"
        if ($EffectiveKey) {
            New-DspkgKeyHeader -KeyHex $EffectiveKey
        }
        elseif ($ResetKeyHeader -or -not (Test-Path -LiteralPath $genHeaderPath)) {
            New-DspkgKeyHeader -Reset
        }

        $targetsToBuild = if ($IsMultiTarget) {
            @(
                @{ Name = "template_release"; DevBuild = "no"; Tests = $false },
                @{ Name = "template_debug"; DevBuild = "yes"; Tests = [bool]$IncludeTests }
            )
        } else {
            $devBuildVal = if ($Target -eq "template_release") { "no" } else { "yes" }
            @(
                @{ Name = $Target; DevBuild = $devBuildVal; Tests = [bool]$IncludeTests }
            )
        }

        $LocalExit = 0
        foreach ($tDef in $targetsToBuild) {
            Write-Status "[Local] Compiling target=$($tDef.Name) dev_build=$($tDef.DevBuild)$(if ($tDef.Tests) { ' tests=true' })..." -ForegroundColor Cyan
            # NOTE: each SCons variable must be a separate process argument.
            $SconsArgs = @("platform=windows", "target=$($tDef.Name)", "dev_build=$($tDef.DevBuild)")
            if ($tDef.Tests) { $SconsArgs += "tests=true" }
            if ($EffectiveKey) { $SconsArgs += "KEEP_KEY_HEADER=1" }
            if ($ResetKeyHeader) { $SconsArgs += "DSPKG_KEY_HEADER_RESET=1" }
            $env:DSPKG_KEY_HEADER_ACTIVE = if ($EffectiveKey) { "1" } else { "0" }

            & scons @SconsArgs 2>&1 | ForEach-Object { Write-Status ([string]$_) }
            $LocalExit = $LASTEXITCODE
            if ($LocalExit -ne 0) {
                break
            }
        }
    }
    finally {
        Pop-Location
        $env:DSPKG_KEY_HEADER_ACTIVE = "0"
        if ($EffectiveKey -and -not $KeepLocalKeyHeader) {
            New-DspkgKeyHeader -Reset
        }
    }
    if ($LocalExit -ne 0) {
        Show-BuildFailure -Step "Local SCons Compilation" -Message "Local SCons build failed with exit code $LocalExit." -Code $LocalExit
    }
    $ArtifactLocalDir = "./project/addons/directstorage_accelerator/bin"
    $LocalManifest = Join-Path $WorkspaceRoot "project/addons/directstorage_accelerator/bin/build_manifest.json"
    if (-not (Test-Path -LiteralPath $LocalManifest)) {
        Write-Status "Warning: build_manifest.json not found at $LocalManifest; SCons may have skipped the manifest step." -ForegroundColor Yellow
    }
    Write-VerboseHost "Local artifacts in $ArtifactLocalDir." -ForegroundColor Green
}
else {
    # --- Remote Mode (REMOTE_BUILD=1): SSH/SCP offload below ---

$RemoteHost = $env:REMOTE_HOST
$RemoteHostFallback = $env:REMOTE_HOST_FALLBACK
$RemoteDir = $env:REMOTE_DIR

if (-not $RemoteHost -or -not $RemoteDir) {
    Show-BuildFailure -Step "Configuration Error" -Message "REMOTE_HOST or REMOTE_DIR is missing in $EnvFilePath" -Code 1
}

try {
    # Header write is inside the protected region so every exit path (including Show-BuildFailure)
    # runs the finally reset below (R8-10, R9-05).
    $genHeaderPath = Join-Path $WorkspaceRoot "src\gen\dspkg_encryption_key.h"
    if ($EffectiveKey) {
        New-DspkgKeyHeader -KeyHex $EffectiveKey
    }
    elseif ($ResetKeyHeader -or -not (Test-Path -LiteralPath $genHeaderPath)) {
        New-DspkgKeyHeader -Reset
    }

# Path separator variants: remote cmd.exe treats / as a switch prefix
# (so D:/ssh-tunnel/... parses /ssh-tunnel as a flag), while tar/scp
# expect forward slashes. Use the Win variant for cmd builtins
# (mkdir/cd/del/echo) and the Tar variant for tar -C / scp paths.
# NOTE: use .Replace() (literal) — -replace is regex and a lone '\' throws
# "InvalidRegularExpression", leaving the variable empty and breaking tar -C.
$RemoteDirWin = $RemoteDir.Replace('/', '\')
$RemoteDirTar = $RemoteDir.Replace('\', '/')

$ArtifactRemoteDir = "$RemoteDirTar/project/addons/directstorage_accelerator/bin"
$ArtifactLocalDir = "./project/addons/directstorage_accelerator/bin"
$RemoteVerifyTar = "$RemoteDirTar/verify_remote.ps1"
$RemoteVerifyWin = "$RemoteDirWin\verify_remote.ps1"

# Unified DLL naming scheme: product stem for the extension binaries
# (dsgd_64.dll / dsgd_64.debug.dll, see ADDON_BINARY_STEM in SConstruct).
# All tar-exclude / SCP-pull / verify patterns below derive from this.
$AddonBinaryStem = "dsgd"

function Invoke-ScpArtifactWithRetry {
    param(
        [string]$RemotePath,
        [string]$LocalDest,
        [int]$MaxRetries = 3
    )
    for ($attempt = 1; $attempt -le $MaxRetries; $attempt++) {
        & scp -4 -C -o BatchMode=yes -o ConnectTimeout=5 "$ActiveHost`:$RemotePath" $LocalDest
        if ($LASTEXITCODE -eq 0) {
            return $true
        }
        if ($attempt -lt $MaxRetries) {
            Write-VerboseHost "Failed to pull $RemotePath (attempt $attempt of $MaxRetries), retrying in 2s..." -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }
    return $false
}

# 0. Check local disk space on Device A
$LocalDriveLetter = (Split-Path -Qualifier (Resolve-Path ".").Path).TrimEnd(':')
if ($LocalDriveLetter) {
    $localDriveItem = Get-PSDrive $LocalDriveLetter -ErrorAction SilentlyContinue
    if ($localDriveItem -and ($localDriveItem.Free / 1MB) -lt 500) {
        $localFreeGb = [math]::Round($localDriveItem.Free / 1GB, 2)
        Show-BuildFailure -Step "Local Disk Space Check" -Message "Local drive ${LocalDriveLetter}: on Device A has only $localFreeGb GB free space (less than 500 MB). Please free up disk space." -Code 1
    }
}

# 1. Probe connectivity (Local Wi-Fi first, fallback to Tailscale)
Write-Status "[1/6] Checking connectivity to primary host ($RemoteHost)..." -ForegroundColor Cyan
$ActiveHost = $null
& ssh -4 -n -o BatchMode=yes -o ConnectTimeout=2 $RemoteHost "exit 0" 2>$null

if ($LASTEXITCODE -eq 0) {
    $ActiveHost = $RemoteHost
    Write-VerboseHost "Connected via local network ($ActiveHost)." -ForegroundColor Green
}
elseif ($RemoteHostFallback) {
    Write-VerboseHost "Local host unreachable. Attempting Tailscale fallback ($RemoteHostFallback)..." -ForegroundColor Yellow
    & ssh -n -o BatchMode=yes -o ConnectTimeout=4 $RemoteHostFallback "exit 0" 2>$null
    if ($LASTEXITCODE -eq 0) {
        $ActiveHost = $RemoteHostFallback
        Write-VerboseHost "Connected via Tailscale ($ActiveHost)." -ForegroundColor Green
    }
    else {
        Show-BuildFailure -Step "Step [1/6] Connectivity" -Message "Neither primary host ($RemoteHost) nor fallback ($RemoteHostFallback) could be reached!" -Code 1
    }
}
else {
    Show-BuildFailure -Step "Step [1/6] Connectivity" -Message "Cannot reach primary host ($RemoteHost) and no fallback is configured!" -Code 1
}

# 2. Ensure target directory exists on Device B
Write-Status "[2/6] Ensuring build directory exists on Device B..." -ForegroundColor Cyan
& ssh -4 -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "if not exist `"$RemoteDirWin`" mkdir `"$RemoteDirWin`""
if ($LASTEXITCODE -ne 0) {
    Show-BuildFailure -Step "Step [2/6] Directory Setup" -Message "Failed to ensure remote build directory exists on $ActiveHost." -Code $LASTEXITCODE
}

# Ensure helper script verify_remote.ps1 is always present on Device B early
$LocalVerifier = Join-Path $PSScriptRoot "verify_remote.ps1"
if (-not (Test-Path -LiteralPath $LocalVerifier)) {
    Show-BuildFailure -Step "Step [2/6] Directory Setup" -Message "Helper script verify_remote.ps1 not found locally at $LocalVerifier." -Code 1
}

$uploadSuccess = $false
$uploadErrDetail = ""
for ($uploadAttempt = 1; $uploadAttempt -le 3; $uploadAttempt++) {
    $uploadOut = & scp -4 -o BatchMode=yes -o ConnectTimeout=5 "$LocalVerifier" "$ActiveHost`:$RemoteVerifyTar" 2>&1
    if ($LASTEXITCODE -eq 0) {
        $uploadSuccess = $true
        break
    }
    $uploadErrDetail = ($uploadOut | Out-String).Trim()
    if ($uploadAttempt -lt 3) {
        Write-VerboseHost "Failed to upload helper script (attempt $uploadAttempt of 3), retrying in 2s..." -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }
}

if (-not $uploadSuccess) {
    $errMessage = "Failed to upload helper script to Device B ($ActiveHost)."
    if ($uploadErrDetail) {
        $errMessage += " SCP Error: $uploadErrDetail"
    }
    Show-BuildFailure -Step "Step [2/6] Directory Setup" -Message $errMessage -Code 1
}

# Check remote disk space on Device B before syncing sources
$RemoteDriveLetter = ($RemoteDirWin -split ':')[0].Trim()
if ($RemoteDriveLetter.Length -eq 1) {
    Write-VerboseHost "Checking remote disk space on drive ${RemoteDriveLetter}: on Device B..." -ForegroundColor Cyan
    $remoteFreeOutput = ssh -4 -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "powershell -NoProfile -Command `"[int64]((Get-PSDrive $RemoteDriveLetter).Free / 1MB)`"" 2>$null
    if ($remoteFreeOutput) {
        $remoteFreeMb = 0
        $parsed = [int64]::TryParse($remoteFreeOutput.Trim(), [ref]$remoteFreeMb)
        if ($parsed -and $remoteFreeMb -gt 0) {
            $remoteFreeGb = [math]::Round($remoteFreeMb / 1024, 2)
            if ($remoteFreeMb -lt 1536) {
                Show-BuildFailure -Step "Step [2/6] Remote Disk Space Check" -Message "Drive ${RemoteDriveLetter}: on Device B ($ActiveHost) has only ${remoteFreeGb} GB free space. SCons compilation requires at least 1.5 GB free space." -Code 1
            }
            elseif ($remoteFreeMb -lt 3072) {
                Write-Status "Warning: Drive ${RemoteDriveLetter}: on Device B ($ActiveHost) has only ${remoteFreeGb} GB free space. Consider cleaning up old build artifacts." -ForegroundColor Yellow
            }
            else {
                Write-VerboseHost "Remote drive ${RemoteDriveLetter}: has ${remoteFreeGb} GB free space." -ForegroundColor Green
            }
        }
    }
}

# Clean orphaned SCons / cl.exe / link.exe processes on Device B BEFORE syncing sources.
# If an earlier build timed out or was interrupted, cl.exe or scons.exe may
# still be running and holding read locks on headers (e.g. ds_profiler.h),
# which causes tar in step [3/6] to fail with "Can't unlink ... Permission denied".
Write-VerboseHost "Cleaning stale build processes on Device B before sync..." -ForegroundColor Cyan
$preSyncOut = & ssh -4 -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost powershell -NoProfile -ExecutionPolicy Bypass -File $RemoteVerifyWin -RemoteDir $RemoteDirWin -CleanOrphans 2>&1
foreach ($cLine in @($preSyncOut)) {
    $cText = $cLine.ToString()
    if ($cText.StartsWith("KILLED_ORPHAN:")) {
        Write-Status "    Terminated orphaned build process before sync: $($cText.Substring(14))" -ForegroundColor Yellow
    }
}

# 3. Stream source files via native tar over SSH (using cmd.exe for raw binary piping)
Write-Status "[3/6] Syncing source files to Device B..." -ForegroundColor Cyan
$tarExcludesList = @(
    "--exclude=.git",
    "--exclude=.vscode",
    "--exclude=.codegraph",
    "--exclude=.opencode",
    "--exclude=.agents",
    "--exclude=.crewmate",
    "--exclude=.godot",
    "--exclude=godot-cpp",
    "--exclude=.env*",
    "--exclude=.sconsign*",
    "--exclude=*.obj",
    "--exclude=*.pdb",
    "--exclude=*.exp",
    "--exclude=*.lib",
    "--exclude=*.ilk",
    "--exclude=*.idb",
    "--exclude=project/addons/directstorage_accelerator/bin/$AddonBinaryStem*",
    "--exclude=project/addons/directstorage_accelerator/bin/~*",
    "--exclude=.keys",
    "--exclude=*.key",
    "--exclude=*.key.txt",
    # Sync-verification manifest rides outside the tarball (uploaded via scp
    # in step [3b/6]); exclude any leftover so it can never be compiled in.
    "--exclude=sync_manifest.tmp",
    # NOTE: do NOT exclude anything matching gdextest here. Windows bsdtar
    # matches --exclude patterns against every path component, so even an
    # anchored-looking "--exclude=./gdextest" strips extern/gdextest/ from the
    # Device B sync and breaks `scons tests=true` (spike defect #1). The
    # repo-root review clone this was meant for does not exist; if one is
    # ever created, syncing it to B is harmless bulk, never a break.
    "--exclude=./wavexecute_tasks"
)
if ($KeyFileRelWin) {
    $tarExcludesList += ("--exclude=" + $KeyFileRelWin.Replace('\', '/'))
    Write-Status "[Sync] Withholding repo-resident key file '$KeyFileRelWin' from Device B transport." -ForegroundColor DarkGray
}
$tarExcludes = $tarExcludesList -join " "

$tarCmd = "tar -czf - $tarExcludes . | ssh -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost ""tar -xzf - -C $RemoteDirTar"""
$syncSuccess = $false
for ($syncAttempt = 1; $syncAttempt -le 2; $syncAttempt++) {
    cmd.exe /c $tarCmd
    if ($LASTEXITCODE -eq 0) {
        $syncSuccess = $true
        break
    }
    if ($syncAttempt -lt 2) {
        Write-VerboseHost "Source sync failed on attempt $syncAttempt, retrying in 2 seconds..." -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }
}

if (-not $syncSuccess) {
    Show-BuildFailure -Step "Step [3/6] Source Sync" -Message "Failed to sync source files to Device B ($ActiveHost)." -Code 1
}

# [3b/6] Verify the sync end-to-end: hash compiled inputs locally, re-hash
# the Device B copies, and fail loudly on any MISSING/HASH mismatch. The
# tar stream exit code alone cannot catch a short sync, and SCons plus its
# cache will happily compile stale sources into a "successful" build.
if (-not $SkipVerify) {
    Write-VerboseHost "[3b/6] Verifying source sync integrity on Device B..." -ForegroundColor Cyan
    $SyncVerifyRoots = @("src", "tests", "thirdparty/gdeflate", "thirdparty/libdeflate", "thirdparty/monocypher", "extern/gdextest/src", "extern/gdextest/tools")
    $SyncVerifyFiles = @("SConstruct", ".gdextest.toml", "extern/gdextest/SConscript")
    $CwdPath = (Resolve-Path ".").Path
    $ManifestEntries = New-Object System.Collections.Generic.List[string]
    foreach ($syncRoot in $SyncVerifyRoots) {
        if (Test-Path -LiteralPath $syncRoot) {
            foreach ($manifestFile in (Get-ChildItem -LiteralPath $syncRoot -Recurse -File)) {
                $relPath = ($manifestFile.FullName.Substring($CwdPath.Length).TrimStart("\", "/")).Replace("\", "/")
                $ManifestEntries.Add($relPath + "|" + (Get-FileHash -LiteralPath $manifestFile.FullName -Algorithm SHA256).Hash)
            }
        }
    }
    foreach ($singleFile in $SyncVerifyFiles) {
        if (Test-Path -LiteralPath $singleFile) {
            $ManifestEntries.Add($singleFile.Replace("\", "/") + "|" + (Get-FileHash -LiteralPath $singleFile -Algorithm SHA256).Hash)
        }
    }
    if (-not (Test-Path -LiteralPath "thirdparty/monocypher")) {
        Show-BuildFailure -Step "SyncVerify" -Message "Required vendored dependency thirdparty/monocypher is missing on Device A." -Code 1
    }
    # Relative name (no drive colon) so scp treats it as a local path.
    $ManifestTmp = "sync_manifest.tmp"
    $ManifestEntries | Set-Content -LiteralPath $ManifestTmp -Encoding Ascii
    $RemoteManifestTar = "$RemoteDirTar/sync_manifest.txt"
    $manifestOut = & scp -4 -o BatchMode=yes -o ConnectTimeout=5 "$ManifestTmp" "$ActiveHost`:$RemoteManifestTar" 2>&1
    Remove-Item $ManifestTmp -Force -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -ne 0) {
        $manifestErr = ($manifestOut | Out-String).Trim()
        Show-BuildFailure -Step "Step [3/6] Sync Verification" -Message "Failed to upload sync manifest to Device B ($ActiveHost). SCP error: $manifestErr" -Code $LASTEXITCODE
    }
    if ($ManifestEntries.Count -eq 0) {
        Show-BuildFailure -Step "Step [3/6] Sync Verification" -Message "Sync manifest is empty; nothing was hashed on Device A. Refusing vacuous pass." -Code 1
    }
    # The verifier runs with spaceless arguments only
    $VerifyOut = & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost powershell -NoProfile -ExecutionPolicy Bypass -File $RemoteVerifyWin -RemoteDir $RemoteDirWin -ManifestName sync_manifest.txt 2>&1
    if ($VerifyOut -notcontains "SYNC_MANIFEST_OK") {
        $detailLines = @($VerifyOut | ForEach-Object { $_.ToString() } | Where-Object { $_.Trim() -ne "" } | Select-Object -First 20) -join "`n"
        Show-BuildFailure -Step "Step [3/6] Sync Verification" -Message "Device B verification failed ($ActiveHost) with no clean OK. Raw remote output:`n$detailLines" -Code 1
    }
    Write-VerboseHost "Source sync verified ($($ManifestEntries.Count) files)." -ForegroundColor Green

    # [3b/6] verified: the keyed header is already on Device B, so scrub Device A now (fast path) (R16-01).
    if ($EffectiveKey -and -not $KeepLocalKeyHeader) {
        New-DspkgKeyHeader -Reset
    }

    # [3c/6] Force test-source freshness on Device B: SCons has silently
    # skipped recompiling changed/new tests/*.cpp (stale DLL, suite runs 0
    # tests), so test TUs are touched to guarantee the next compile sees
    # them. Quoteless invocation, same as [3b/6].
    Write-VerboseHost "[3c/6] Touching test sources on Device B..." -ForegroundColor Cyan
    $TouchOut = & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost powershell -NoProfile -ExecutionPolicy Bypass -File $RemoteVerifyWin -RemoteDir $RemoteDirWin -TouchTests 2>&1
    $TouchCount = 0
    foreach ($touchLine in @($TouchOut)) {
        $touchText = $touchLine.ToString()
        if ($touchText.StartsWith("TOUCHED_TESTS=")) {
            [int]::TryParse($touchText.Substring(14), [ref]$TouchCount) | Out-Null
        }
    }
    if ($TouchCount -eq 0) {
        $touchDetail = @($TouchOut | ForEach-Object { $_.ToString() } | Where-Object { $_.Trim() -ne "" } | Select-Object -First 10) -join "`n"
        Show-BuildFailure -Step "Step [3/6] Test Source Refresh" -Message "Device B test-source touch reported zero files ($ActiveHost). Raw remote output:`n$touchDetail" -Code 1
    }
    Write-VerboseHost "Test sources refreshed ($TouchCount file(s))." -ForegroundColor Green
}

# 4. Prep + SCons build on Device B. Prep (orphan cleanup + marker reset) runs
# FIRST in a single SSH round-trip, and [4/6] Compiling prints only after prep
# finishes — so a slow cleanup is never mistaken for a slow compile.
$TestsFlag = if ($IncludeTests) { " tests=true" } else { "" }
$RemoteMarkerWin = "$RemoteDirWin\gdextest_scons.ok"

# [4a/6] Clean orphaned SCons / cl.exe processes from interrupted runs for this
# project, and reset the success marker in the same remote invocation (saves
# 2 SSH handshakes + 2 powershell cold starts vs. del/type via cmd.exe).
Write-Status "[4/6] Cleaning stale build processes + resetting marker on Device B..." -ForegroundColor Cyan
$prepTimer = [System.Diagnostics.Stopwatch]::StartNew()
$PrepOut = & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost powershell -NoProfile -ExecutionPolicy Bypass -File $RemoteVerifyWin -RemoteDir $RemoteDirWin -CleanOrphans -ResetMarker 2>&1
$prepTimer.Stop()
$prepSecs = [int]$prepTimer.Elapsed.TotalSeconds
$killedCount = 0
$markerOk = $false
foreach ($cLine in @($PrepOut)) {
    $cText = $cLine.ToString()
    if ($cText.StartsWith("KILLED_ORPHAN:")) {
        Write-Status "    Terminated orphaned build process: $($cText.Substring(14))" -ForegroundColor Yellow
    }
    elseif ($cText.StartsWith("CLEANED_ORPHANS_COUNT=")) {
        [int]::TryParse($cText.Substring(22), [ref]$killedCount) | Out-Null
    }
    elseif ($cText -eq "MARKER_RESET_OK") {
        $markerOk = $true
    }
}
if (-not $markerOk) {
    $prepDetail = @($PrepOut | ForEach-Object { $_.ToString() } | Where-Object { $_.Trim() -ne "" } | Select-Object -First 10) -join "`n"
    Show-BuildFailure -Step "Step [4/6] Prep (Clean + Marker Reset)" -Message "Prep on Device B ($ActiveHost) failed after ${prepSecs}s (killed=$killedCount). Raw remote output:`n$prepDetail" -Code 1
}
Write-Status "    [4/6 Prep done in ${prepSecs}s, killed $killedCount orphan(s). Compiling target=$Target...]" -ForegroundColor DarkCyan
# Exit status cannot ride the ssh code: OpenSSH's cmd shell returns 0 even
# when SCons fails, so a marker file written only after a successful scons
# is the reliable signal (reset above, checked below).
# SCons output is captured and monitored. We use temporary output files with
# Start-Process so the script can print a 1-minute heartbeat if compilation
# is still in progress, preventing automated monitors from suspecting a hang.
$sconsOutFile = Join-Path $env:TEMP ("scons_remote_out_" + [System.Guid]::NewGuid().ToString() + ".log")
$sconsErrFile = Join-Path $env:TEMP ("scons_remote_err_" + [System.Guid]::NewGuid().ToString() + ".log")
$keepArg = if ($EffectiveKey) { " KEEP_KEY_HEADER=1" } elseif ($ResetKeyHeader) { " DSPKG_KEY_HEADER_RESET=1" } else { "" }
if ($IsMultiTarget) {
    $testsArgDebug = if ($IncludeTests) { " tests=true" } else { "" }
    $sconsRelCmd = "call scons platform=windows target=template_release dev_build=no$keepArg"
    $sconsDbgCmd = "call scons platform=windows target=template_debug dev_build=yes$testsArgDebug$keepArg"
    $remoteCmd = "cd /d `"$RemoteDirWin`" && $sconsRelCmd && $sconsDbgCmd && echo ok> `"$RemoteMarkerWin`""
}
else {
    $devBuildVal = if ($Target -eq "template_release") { "no" } else { "yes" }
    $remoteCmd = "cd /d `"$RemoteDirWin`" && call scons platform=windows target=$Target dev_build=$devBuildVal$TestsFlag$keepArg && echo ok> `"$RemoteMarkerWin`""
}

$sconsArgs = @(
    "-n",
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=10",
    "-o", "ServerAliveInterval=5",
    "-o", "ServerAliveCountMax=3",
    $ActiveHost,
    $remoteCmd
)
$sconsProc = Start-Process -FilePath "ssh" -ArgumentList $sconsArgs -NoNewWindow -PassThru -RedirectStandardOutput $sconsOutFile -RedirectStandardError $sconsErrFile

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$lastHeartbeatMin = 0
while (-not $sconsProc.WaitForExit(10000)) {
    $elapsedMin = [int][Math]::Floor($stopwatch.Elapsed.TotalMinutes)
    if ($elapsedMin -gt $lastHeartbeatMin) {
        $lastHeartbeatMin = $elapsedMin
        Write-Status "    [4/6 Still compiling on Device B... ($lastHeartbeatMin min elapsed)]" -ForegroundColor DarkCyan
    }
}
$stopwatch.Stop()

$sconsLines = @()
if (Test-Path -LiteralPath $sconsOutFile) {
    $sconsLines += @(Get-Content -LiteralPath $sconsOutFile)
    Remove-Item -LiteralPath $sconsOutFile -Force -ErrorAction SilentlyContinue
}
if (Test-Path -LiteralPath $sconsErrFile) {
    $sconsLines += @(Get-Content -LiteralPath $sconsErrFile)
    Remove-Item -LiteralPath $sconsErrFile -Force -ErrorAction SilentlyContinue
}
if ($Verbose -and $sconsLines) {
    $sconsLines | ForEach-Object { Write-Status $_ }
}

# Distinguish network/SSH drop (exit code 255 or host unreachable) from actual compilation error
$sshDropped = ($sconsProc.ExitCode -eq 255)
if (-not $sshDropped) {
    & ssh -n -o BatchMode=yes -o ConnectTimeout=3 $ActiveHost "exit 0" 2>$null
    if ($LASTEXITCODE -ne 0) {
        $sshDropped = $true
    }
}

if ($sshDropped) {
    if ($sconsLines) {
        Write-Status "--- SCons Output (before connection was lost) ---" -ForegroundColor Yellow
        $sconsLines | Select-Object -Last 15 | ForEach-Object { Write-Status $_ }
    }
    Show-BuildFailure -Step "Step [4/6] SSH Disconnected" -Message "SSH connection to Device B ($ActiveHost) dropped or timed out during compilation (SSH Exit Code: $($sconsProc.ExitCode))." -Code 255
}

& ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "type `"$RemoteMarkerWin`" >nul 2>nul"
$markerMissing = $LASTEXITCODE -ne 0
$sconsFailed = @($sconsLines | Where-Object { $_ -match "terminated because of errors|scons: \*\*\* \[|error C[0-9]+|error LNK[0-9]+" }).Count -gt 0
if ($markerMissing -or $sconsFailed) {
    if ($sconsLines) {
        $errorLines = $sconsLines | Where-Object { $_ -match "error[:\s]|fatal|failed|exception" }
        if ($errorLines) {
            Write-Status "--- SCons Compilation Errors ---" -ForegroundColor Red
            $errorLines | Select-Object -Last 30 | ForEach-Object { Write-Status $_ }
        }
        else {
            Write-Status "--- SCons Output (tail) ---" -ForegroundColor Red
            $sconsLines | Select-Object -Last 30 | ForEach-Object { Write-Status $_ }
        }
    }
    $why = if ($sconsFailed) { "SCons reported compilation errors" } else { "missing success marker (SCons did not complete successfully)" }
    Show-BuildFailure -Step "Step [4/6] SCons Compilation" -Message "Compilation failed on Device B ($ActiveHost). Detected via $why." -Code 1
}

# 5. Pull back compiled binary and debug symbols to Device A
Write-Status "[5/6] Pulling artifacts back to Device A (compressed)..." -ForegroundColor Cyan
if (!(Test-Path $ArtifactLocalDir)) {
    New-Item -ItemType Directory -Force -Path $ArtifactLocalDir | Out-Null
}

# Clean any remote hot-reload temporary DLLs before pulling
& ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "if exist `"$RemoteDirWin\project\addons\directstorage_accelerator\bin\~*`" del /q `"$RemoteDirWin\project\addons\directstorage_accelerator\bin\~*`"" 2>$null

# Pull runtime DLLs and manifest with on-the-fly compression (-C) and bounded retries
if (-not (Invoke-ScpArtifactWithRetry -RemotePath "$ArtifactRemoteDir/$AddonBinaryStem*.dll" -LocalDest "$ArtifactLocalDir/")) {
    Show-BuildFailure -Step "Step [5/6] Pull Artifacts" -Message "Failed to copy compiled $AddonBinaryStem*.dll from Device B ($ActiveHost)." -Code 1
}
if (-not (Invoke-ScpArtifactWithRetry -RemotePath "$ArtifactRemoteDir/dstorage*.dll" -LocalDest "$ArtifactLocalDir/")) {
    Show-BuildFailure -Step "Step [5/6] Pull Artifacts" -Message "Failed to copy dstorage*.dll from Device B ($ActiveHost)." -Code 1
}
if ($IncludeTests) {
    if (!(Test-Path "./bin")) {
        New-Item -ItemType Directory -Force -Path "./bin" | Out-Null
    }
    $targetForTestPattern = if ($IsMultiTarget) { "template_debug" } else { $Target }
    $targetTestPattern = "libdirectstorage_tests*.$targetForTestPattern*.dll"
    Get-ChildItem -Path "./bin" -Filter $targetTestPattern -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    if (-not (Invoke-ScpArtifactWithRetry -RemotePath "$RemoteDirTar/bin/$targetTestPattern" -LocalDest "./bin/")) {
        Show-BuildFailure -Step "Step [5/6] Pull Test Artifacts" -Message "Failed to copy test .dll ($targetTestPattern) from Device B ($ActiveHost). The remote test build likely failed." -Code 1
    }
}
& scp -C -o BatchMode=yes -o ConnectTimeout=5 "$ActiveHost`:$ArtifactRemoteDir/build_manifest.json" "$ArtifactLocalDir/" 2>$null

# [5b/6] Verify pulled artifacts equal the Device B files (transfer
# integrity). A truncated scp must never pass as a fresh build.
if (-not $SkipVerify) {
    Write-VerboseHost "[5b/6] Verifying pulled artifact hashes against Device B..." -ForegroundColor Cyan
    $PullChecks = @(
        @{ Pattern = "$AddonBinaryStem*.dll"; LocalDir = $ArtifactLocalDir; Required = $true },
        @{ Pattern = "dstorage*.dll"; LocalDir = $ArtifactLocalDir; Required = $true },
        @{ Pattern = "build_manifest.json"; LocalDir = $ArtifactLocalDir; Required = [bool]($EffectiveKey -or $RequireManifest) }
    )
    if ($IncludeTests) {
        $targetForTestPattern = if ($IsMultiTarget) { "template_debug" } else { $Target }
        $targetTestPattern = "libdirectstorage_tests*.$targetForTestPattern*.dll"
        $PullChecks += @{ Pattern = $targetTestPattern; LocalDir = "./bin"; Required = $true }
    }
    # Remote hashes come from the verifier script (quoteless invocation, same
    # as step [3b/6]) so no nested quoting is needed here either.
    $ArtOut = & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost powershell -NoProfile -ExecutionPolicy Bypass -File $RemoteVerifyWin -RemoteDir $RemoteDirWin -Artifacts 2>&1
    $RemoteArtifacts = @{}
    foreach ($artLine in @($ArtOut)) {
        $artText = $artLine.ToString()
        if ($artText.StartsWith("ARTIFACT:")) {
            $artRest = $artText.Substring(9)
            $artSep = $artRest.IndexOf("|")
            if ($artSep -gt 0) {
                $RemoteArtifacts[$artRest.Substring(0, $artSep)] = $artRest.Substring($artSep + 1)
            }
        }
    }
    foreach ($pullCheck in $PullChecks) {
        $matchedNames = @($RemoteArtifacts.Keys | Where-Object { $_ -like $pullCheck.Pattern })
        if ($matchedNames.Count -eq 0 -and $pullCheck.Required) {
            $rawRemote = @($ArtOut | ForEach-Object { $_.ToString() } | Where-Object { $_.Trim() -ne "" } | Select-Object -First 10) -join "`n"
            Show-BuildFailure -Step "Step [5/6] Artifact Verification" -Message "No artifacts matched on Device B for pattern $($pullCheck.Pattern). Raw remote output:`n$rawRemote" -Code 1
        }
        foreach ($matchedName in $matchedNames) {
            $LocalFile = Join-Path $pullCheck.LocalDir $matchedName
            if (-not (Test-Path -LiteralPath $LocalFile)) {
                Show-BuildFailure -Step "Step [5/6] Artifact Verification" -Message "Pulled artifact missing locally: $matchedName." -Code 1
            }
            $LocalHash = (Get-FileHash -LiteralPath $LocalFile -Algorithm SHA256).Hash
            if ($LocalHash -ne $RemoteArtifacts[$matchedName]) {
                Show-BuildFailure -Step "Step [5/6] Artifact Verification" -Message "Hash mismatch on $matchedName : transfer corrupted." -Code 1
            }
        }
    }
    Write-VerboseHost "Pulled artifacts verified." -ForegroundColor Green
}

# Pull large PDB debug symbols only when explicitly requested
if ($IncludePdb) {
    Write-VerboseHost "Pulling debug symbols (.pdb) with compression..." -ForegroundColor Cyan
    & scp -C -o BatchMode=yes -o ConnectTimeout=5 "$ActiveHost`:$ArtifactRemoteDir/*.pdb" "$ArtifactLocalDir/" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-VerboseHost "Warning: Failed to download .pdb debug symbols." -ForegroundColor Yellow
    }
}
else {
    Write-VerboseHost "Skipping .pdb download (use -IncludePdb if attaching a C++ debugger)." -ForegroundColor DarkGray
}

Write-VerboseHost "Artifacts updated on Device A." -ForegroundColor Green
}
finally {
    # Device A: abort-safe reset, covering every termination path that precedes the
    # post-[3b/6] reset (SSH drop, SCons failure, missing marker) (R14-02).
    # -KeepLocalKeyHeader deliberately retains the header at rest.
    $env:DSPKG_KEY_HEADER_ACTIVE = "0"
    if ($EffectiveKey -and -not $KeepLocalKeyHeader) {
        New-DspkgKeyHeader -Reset
    }
    if ($ActiveHost -and -not $KeepRemoteKeyHeader) {
        $headerWin = "$RemoteDirWin\src\gen\dspkg_encryption_key.h"
        $remotePurgeTargets = "`"$headerWin*`" `"$headerWin.tmp*`""
        if ($KeyFileRelWin) { $remotePurgeTargets += " `"$RemoteDirWin\$KeyFileRelWin`"" }
        & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "del /f /q $remotePurgeTargets 2>nul" 2>&1 | Out-Null
    }
}
} # End Remote Mode (REMOTE_BUILD=1)

# 5b. GDScript Syntax Verification
if (-not $SkipVerify) {
    Write-Status "[verify] Validating addon GDScript syntax..." -ForegroundColor Cyan
    $checkScript = Join-Path $PSScriptRoot "check_scripts.ps1"
    if (Test-Path -LiteralPath $checkScript) {
        & powershell -ExecutionPolicy Bypass -File $checkScript
        if ($LASTEXITCODE -ne 0) {
            Show-BuildFailure -Step "GDScript Verification" -Message "One or more addon GDScript files failed syntax validation. Aborting before sync." -Code 1
        }
    }
}

# 6. Post-build synchronization to external project (e.g. TPS demo)
# Environment-driven: syncs whenever SYNC_ADDON_DIR is defined.
$EnableSyncVal = if ($env:ENABLE_POST_BUILD_ADDON_SYNC) { $env:ENABLE_POST_BUILD_ADDON_SYNC } `
    elseif ($env:ENABLE_ADDON_SYNC) { $env:ENABLE_ADDON_SYNC } `
    elseif ($env:DIRECTSTORAGE_ENABLE_EXTERNAL_SYNC) { $env:DIRECTSTORAGE_ENABLE_EXTERNAL_SYNC } `
    else { "1" }

$ShouldSync = ($EnableSyncVal.Trim().ToLower() -notin @("0", "false", "no", "off"))
$SyncTargetDir = if ($env:SYNC_ADDON_DIR) { $env:SYNC_ADDON_DIR }
$copiedCount = 0
$orphanedSyncFiles = [System.Collections.Generic.List[string]]::new()

if ($ShouldSync) {
    if (-not $SyncTargetDir) {
        Write-VerboseHost "Skipping post-build sync: SYNC_ADDON_DIR is not configured." -ForegroundColor DarkGray
    }
    else {
        Write-Status "[6/6] Syncing addon to external project ($SyncTargetDir)..." -ForegroundColor Cyan
        $LocalAddonDir = (Resolve-Path (Join-Path $PSScriptRoot "..\project\addons\directstorage_accelerator")).Path

        if (Test-Path $LocalAddonDir) {
            if (-not (Test-Path $SyncTargetDir)) {
                New-Item -ItemType Directory -Force -Path $SyncTargetDir | Out-Null
            }

            function Copy-SafeFile {
                param(
                    [string]$SourcePath,
                    [string]$DestinationPath
                )
                if ($DestinationPath.ToLower().EndsWith(".dll")) {
                    $oldPath = "$DestinationPath.old"
                    if (Test-Path $oldPath) {
                        try { Remove-Item -Force -Path $oldPath -ErrorAction SilentlyContinue } catch { $null = $_ }
                    }
                }
                try {
                    Copy-Item -Path $SourcePath -Destination $DestinationPath -Force -ErrorAction Stop
                }
                catch {
                    if ((Test-Path $DestinationPath) -and $DestinationPath.ToLower().EndsWith(".dll")) {
                        $oldPath = "$DestinationPath.old"
                        if (Test-Path $oldPath) {
                            try { Remove-Item -Force -Path $oldPath -ErrorAction SilentlyContinue } catch { $null = $_ }
                        }
                        if (Test-Path $oldPath) {
                            $timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
                            $oldPath = "$DestinationPath.$timestamp.old"
                        }
                        try {
                            Move-Item -Path $DestinationPath -Destination $oldPath -Force -ErrorAction SilentlyContinue
                        }
                        catch { $null = $_ }
                        Copy-Item -Path $SourcePath -Destination $DestinationPath -Force -ErrorAction SilentlyContinue
                    }
                    else {
                        throw $_
                    }
                }
            }

            $copiedCount = 0
            Get-ChildItem -Path $LocalAddonDir -Recurse | ForEach-Object {
                $item = $_
                $relPath = $item.FullName.Substring($LocalAddonDir.Length).TrimStart("\", "/")

                # Exclude temporary, old, and dev build files
                if ($item.Name.StartsWith("~") -or $item.Name.EndsWith(".tmp") -or $item.Name.EndsWith(".old")) {
                    return
                }
                if ($relPath.StartsWith("bin") -and $item.Name.Contains(".dev.")) {
                    return
                }

                $destPath = Join-Path $SyncTargetDir $relPath
                if ($item.PSIsContainer) {
                    if (-not (Test-Path $destPath)) {
                        New-Item -ItemType Directory -Force -Path $destPath | Out-Null
                    }
                }
                else {
                    $destParent = Split-Path $destPath -Parent
                    if (-not (Test-Path $destParent)) {
                        New-Item -ItemType Directory -Force -Path $destParent | Out-Null
                    }

                    $needCopy = $false
                    if (-not (Test-Path $destPath)) {
                        $needCopy = $true
                    }
                    else {
                        $destItem = Get-Item $destPath
                        if ($item.Length -ne $destItem.Length -or $item.LastWriteTime -gt $destItem.LastWriteTime.AddSeconds(1)) {
                            $needCopy = $true
                        }
                    }

                    if ($needCopy) {
                        try {
                            Copy-SafeFile -SourcePath $item.FullName -DestinationPath $destPath
                            $copiedCount++
                        }
                        catch {
                            Write-Status "Warning: Failed to copy $($item.Name): $_" -ForegroundColor Yellow
                        }
                    }
                }
            }

            # Track orphaned files in destination that no longer exist in source addon
            Get-ChildItem -Path $SyncTargetDir -Recurse -File | ForEach-Object {
                $destFile = $_
                $relPath = $destFile.FullName.Substring($SyncTargetDir.Length).TrimStart("\", "/")

                # Exclude binaries, temporary, and old files
                if ($relPath.StartsWith("bin") -or $destFile.Name.EndsWith(".old") -or $destFile.Name.EndsWith(".tmp")) {
                    return
                }

                $sourcePath = Join-Path $LocalAddonDir $relPath
                if (-not (Test-Path $sourcePath)) {
                    $orphanedSyncFiles.Add($destFile.FullName)
                }
            }

            # Attempt to clean up unlocked .old files in destination bin
            $destBin = Join-Path $SyncTargetDir "bin"
            if (Test-Path $destBin) {
                Get-ChildItem -Path $destBin -Filter "*.old" -ErrorAction SilentlyContinue | ForEach-Object {
                    try { Remove-Item -Force -Path $_.FullName -ErrorAction SilentlyContinue } catch { $null = $_ }
                }
            }

            # Update build_manifest.json on Device A using Python for clean 2-space indent (avoiding PS 5.1 column spacing and BOM)
            $manifestLocalPath = Join-Path $LocalAddonDir "bin\build_manifest.json"
            if (Test-Path $manifestLocalPath) {
                try {
                    $pyCmd = "import json, sys; p = sys.argv[1]; d = json.load(open(p, encoding='utf-8-sig')); d.setdefault('post_build_sync', {}).setdefault('tps_demo_addon', {})['synced'] = True; d['post_build_sync']['tps_demo_addon']['copied_files_count'] = int(sys.argv[2]); json.dump(d, open(p, 'w', encoding='utf-8'), indent=2)"
                    & python -c $pyCmd $manifestLocalPath $copiedCount

                    $destManifest = Join-Path $destBin "build_manifest.json"
                    if (Test-Path $destBin) {
                        Copy-Item -Path $manifestLocalPath -Destination $destManifest -Force
                    }
                }
                catch {
                    $null = $_
                }
            }

            if ($copiedCount -gt 0) {
                Write-VerboseHost "Post-build sync complete: Updated $copiedCount file(s) -> $SyncTargetDir" -ForegroundColor Green
            }
            else {
                Write-VerboseHost "Post-build sync: Target $SyncTargetDir is already up to date." -ForegroundColor DarkGray
            }
        }
    }
}
else {
    Write-VerboseHost "Post-build addon sync is disabled (ENABLE_POST_BUILD_ADDON_SYNC=0)." -ForegroundColor DarkGray
}

# Advisory deployment audit: reports files present in a deployed addon folder
# that its .gdextension does not declare (stale binaries from an older naming
# scheme, hand copies, ...). Read-only - nothing is deleted or moved here.
Write-Status "[audit] Scanning deployed addon directories..." -ForegroundColor Cyan
$DeployAuditTargets = @($DefaultAddonDir) + @(Get-ConfiguredAddonDir)
$postBuildAuditError = Invoke-DeployAudit -AddonDirs $DeployAuditTargets -Json:$Json -RequireManifest:([bool]($EffectiveKey -or $RequireManifest))
if ($postBuildAuditError -and ($EffectiveKey -or $RequireManifest)) {
    Show-BuildFailure -Step "Audit" -Message "Keyed/release build deployment audit failed due to binary/manifest key-id mismatch or error." -Code 1
}

$syncMsg = if ($ShouldSync -and $copiedCount -gt 0) { " (Synced $copiedCount file(s))" } else { "" }
Show-BuildSuccess -Message "All build and sync steps completed successfully!$syncMsg"

if ($orphanedSyncFiles -and $orphanedSyncFiles.Count -gt 0) {
    Write-Host ""
    Write-Host "WARNING: Found $($orphanedSyncFiles.Count) orphaned file(s) in sync target ($SyncTargetDir) that no longer exist in source addon:" -ForegroundColor Yellow
    foreach ($orphan in $orphanedSyncFiles) {
        Write-Host "  - $orphan" -ForegroundColor Yellow
    }
    Write-Host "Please remove them manually if they are no longer needed." -ForegroundColor Yellow
    Write-Host ""
}


