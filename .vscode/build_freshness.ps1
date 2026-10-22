<#
.SYNOPSIS
    Answers "was the deployed extension binary built from the sources I have now?"
    without running a build.

.DESCRIPTION
    Default mode (-Check) is read-only: it re-hashes the build inputs on this
    machine, compares them against the stamp written after the last successful
    build, and verifies the deployed DLL hashes still match that stamp.

    Why not `scons -n` / a dry run: a dry run reports on the tree of the machine
    it runs on. On a machine with PREVENT_BUILD_ON_THIS_DEVICE=1 the local
    .sconsign never tracks the remote build, so it reports a full rebuild; on
    the build machine it reports "up to date" for the tree that was last synced
    there, even when newer local edits were never sent over. Neither answer is
    the one that matters. Comparing this machine's current inputs against the
    record captured at build time is.

    -Record is called by build_remote.ps1 at the end of a successful build.
    Do not run it by hand against a stale tree: it would stamp old artifacts as
    fresh.

.PARAMETER Target
    Build configuration to check or record. Multi-target values expand to both
    template_release and template_debug.

.PARAMETER IncludeTests
    Recorded as metadata (only the debug target can carry tests). Test-library
    freshness is deliberately not tracked: build_remote.ps1 touches test
    sources before every build, so test artifacts are never "unchanged".

.PARAMETER Record
    Write the stamp instead of checking it.

.PARAMETER StampPath
    Stamp location. Defaults to <repo>/.cache/build_freshness.json (gitignored).

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory.

.NOTES
    Exit codes: 0 = fresh, 1 = stale or no usable record, 2 = the check itself
    failed. Anything that cannot be verified is reported as stale, never fresh.

    Known limits:
      - A changed DSPKG encryption key is not detected by the input set (the
        generated key header is rewritten by every build and reset afterwards).
        The key id that produced the recorded binaries is printed instead.
      - godot-cpp is fingerprinted by commit + working-tree status rather than
        by content (2443 files / ~72 MB); edits inside a dirty tree that leave
        the status hash unchanged are not seen.
#>
[CmdletBinding()]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute("PSAvoidUsingWriteHost", "", Justification = "Interactive console report; the rest of .vscode uses Write-Host for colored build status output.")]
param(
    [Parameter(Mandatory = $false)]
    [ValidateSet("template_debug", "template_release", "template_debug_and_release", "all")]
    [string]$Target = "template_debug",

    [Parameter(Mandatory = $false)]
    [switch]$IncludeTests,

    [Parameter(Mandatory = $false)]
    [switch]$Record,

    [Parameter(Mandatory = $false)]
    [string]$StampPath = "",

    [Parameter(Mandatory = $false)]
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"

# --- Input set ---------------------------------------------------------------
# These are the files that can change the compiled extension DLL. Roots mirror
# $SyncVerifyRoots / $SyncVerifyFiles in build_remote.ps1, plus two that the sync
# list does not carry:
#   - doc_classes/*.xml is compiled into src/gen/doc_data.gen.cpp
#   - thirdparty/directstorage supplies the headers and import library
# tests/ and extern/gdextest/** are intentionally absent: they only build the
# gdextest library, not the extension DLL (see -IncludeTests notes).
$InputRoots = @(
    "src",
    "thirdparty/gdeflate",
    "thirdparty/libdeflate",
    "thirdparty/monocypher",
    "thirdparty/directstorage",
    "project/addons/directstorage_accelerator/doc_classes"
)
$InputFiles = @("SConstruct")

# --- Outputs -----------------------------------------------------------------
# Artifact names come from SCons' own build_manifest.json (targets/<file>/
# target_type), so this script never hardcodes the naming scheme. The fallback
# only matters when the manifest is unavailable.
$AddonBinRel = "project/addons/directstorage_accelerator/bin"
$StampSchema = 1

function Write-Line {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $false)][string]$Color = "Gray"
    )
    Write-Host $Text -ForegroundColor $Color
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha.Dispose()
    }
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string]$Text)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text)))).Replace("-", "")
    }
    finally {
        $sha.Dispose()
    }
}

function Get-BuildInputFingerprint {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$Roots,
        [Parameter(Mandatory = $true)][string[]]$Files
    )

    $map = [ordered]@{}
    foreach ($relRoot in $Roots) {
        $absRoot = Join-Path $Root $relRoot
        if (-not (Test-Path -LiteralPath $absRoot)) {
            continue
        }
        $filesFound = @(Get-ChildItem -LiteralPath $absRoot -Recurse -File -Force -ErrorAction SilentlyContinue | Sort-Object FullName)
        foreach ($file in $filesFound) {
            $rel = $file.FullName.Substring($Root.Length).TrimStart("\", "/").Replace("\", "/")
            # src/gen is generated by every build (doc_data.gen.cpp and the key
            # header). The key header is also reset after a keyed build, so it can
            # never compare equal to a post-build stamp.
            if ($rel -like "src/gen/*") {
                continue
            }
            if ($rel -like "*.gen.cpp" -or $rel -like "*.gen.h") {
                continue
            }
            $map[$rel] = Get-FileSha256 -Path $file.FullName
        }
    }
    foreach ($rel in $Files) {
        $abs = Join-Path $Root $rel
        if (Test-Path -LiteralPath $abs) {
            $map[$rel.Replace("\", "/")] = Get-FileSha256 -Path $abs
        }
    }
    return $map
}

function Get-GodotCppFingerprint {
    param([Parameter(Mandatory = $true)][string]$Root)

    $dir = Join-Path $Root "godot-cpp"
    if (-not (Test-Path -LiteralPath (Join-Path $dir ".git"))) {
        return [ordered]@{ head = ""; dirty = "missing" }
    }

    $head = @()
    $status = @()
    try {
        $head = @(& git -C $dir rev-parse HEAD 2>$null)
        $status = @(& git -C $dir status --porcelain 2>$null)
    }
    catch {
        return [ordered]@{ head = ""; dirty = "unavailable" }
    }

    $headText = ""
    if ($head.Count -gt 0 -and $head[0]) {
        $headText = ([string]$head[0]).Trim()
    }
    $dirtyHash = ""
    if ($status.Count -gt 0) {
        $dirtyText = (@($status) | Sort-Object) -join "`n"
        if ($dirtyText) {
            $dirtyHash = Get-TextSha256 -Text $dirtyText
        }
    }
    return [ordered]@{ head = $headText; dirty = $dirtyHash }
}

function Get-TargetArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$BinDir,
        [Parameter(Mandatory = $true)][string]$Config
    )

    $names = @()
    $manifestPath = Join-Path $BinDir "build_manifest.json"
    if (Test-Path -LiteralPath $manifestPath) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
            if ($manifest.targets) {
                foreach ($prop in $manifest.targets.PSObject.Properties) {
                    if ($prop.Value.target_type -eq $Config -and $prop.Value.exists) {
                        $names += [string]$prop.Name
                    }
                }
            }
        }
        catch {
            $null = $_
        }
    }
    if ($names.Count -eq 0) {
        # Fallback only: SConstruct names the debug output with the debug suffix.
        if ($Config -eq "template_debug") {
            $names = @("dsgd_64.debug.dll")
        }
        else {
            $names = @("dsgd_64.dll")
        }
    }
    return $names
}

function Get-TargetEncryption {
    param(
        [Parameter(Mandatory = $true)][string]$BinDir,
        [Parameter(Mandatory = $true)][string]$ArtifactName
    )

    $result = [ordered]@{ enabled = $false; key_id = "none" }
    $manifestPath = Join-Path $BinDir "build_manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        return $result
    }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($manifest.targets) {
            $prop = $manifest.targets.PSObject.Properties[$ArtifactName]
            if ($prop -and $prop.Value.encryption) {
                $result.enabled = [bool]$prop.Value.encryption.enabled
                if ($prop.Value.encryption.key_id) {
                    $result.key_id = [string]$prop.Value.encryption.key_id
                }
            }
        }
    }
    catch {
        $null = $_
    }
    return $result
}

function Read-StampDocument {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }
    try {
        $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    }
    catch {
        return $null
    }
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return $null
    }
    try {
        return ($raw | ConvertFrom-Json)
    }
    catch {
        return $null
    }
}

function Invoke-Record {
    param(
        [Parameter(Mandatory = $true)][string]$Repo,
        [Parameter(Mandatory = $true)][string]$BinDir,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Configs,
        [Parameter(Mandatory = $true)][bool]$TestsRequested
    )

    $existing = Read-StampDocument -Path $Path
    $records = [ordered]@{}
    if ($existing -and $existing.records) {
        foreach ($prop in $existing.records.PSObject.Properties) {
            $records[$prop.Name] = $prop.Value
        }
    }
    elseif (Test-Path -LiteralPath $Path) {
        Write-Line ("[record] existing stamp is unreadable and will be replaced: {0}" -f $Path) "Yellow"
    }

    $inputs = Get-BuildInputFingerprint -Root $Repo -Roots $InputRoots -Files $InputFiles
    if ($inputs.Count -eq 0) {
        throw "No build inputs were found under $Repo; refusing to record an empty fingerprint."
    }
    $godotCpp = Get-GodotCppFingerprint -Root $Repo
    $stampTime = (Get-Date).ToString("o")

    foreach ($config in $Configs) {
        $artifactNames = @(Get-TargetArtifact -BinDir $BinDir -Config $config)
        $outputs = [ordered]@{}
        foreach ($name in $artifactNames) {
            $abs = Join-Path $BinDir $name
            if (Test-Path -LiteralPath $abs) {
                $outputs[$name] = Get-FileSha256 -Path $abs
            }
        }
        if ($outputs.Count -eq 0) {
            Write-Line ("[record] warning: no artifact found for {0}; nothing recorded for it" -f $config) "Yellow"
            continue
        }

        $testsValue = $false
        if ($config -eq "template_debug") {
            $testsValue = $TestsRequested
        }
        $firstArtifact = @($outputs.Keys)[0]
        $encryption = Get-TargetEncryption -BinDir $BinDir -ArtifactName $firstArtifact

        $records[$config] = [ordered]@{
            built_at    = $stampTime
            dev_build   = ($config -ne "template_release")
            tests       = $testsValue
            schema      = $StampSchema
            inputs      = $inputs
            outputs     = $outputs
            godot_cpp   = $godotCpp
            encryption  = $encryption
        }
        Write-Line ("[record] {0}: {1} input file(s), artifact {2}" -f $config, $inputs.Count, ($outputs.Keys -join ", ")) "DarkGray"
    }

    $parent = Split-Path -Path $Path -Parent
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    $document = [ordered]@{
        schema    = $StampSchema
        generator = ".vscode/build_freshness.ps1"
        records   = $records
    }
    $json = $document | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
    Write-Line ("[record] stamp written: {0}" -f $Path) "Green"
}

function Invoke-Check {
    param(
        [Parameter(Mandatory = $true)][string]$Repo,
        [Parameter(Mandatory = $true)][string]$BinDir,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Configs,
        [Parameter(Mandatory = $true)][bool]$TestsRequested
    )

    $stamp = Read-StampDocument -Path $Path
    if (-not $stamp -or -not $stamp.records) {
        Write-Line ("[STALE] No usable build record at {0}" -f $Path) "Yellow"
        Write-Line "        Run a build (build_remote.ps1 or full_build.ps1) to create one." "DarkGray"
        return 1
    }

    $current = Get-BuildInputFingerprint -Root $Repo -Roots $InputRoots -Files $InputFiles
    $godotCpp = Get-GodotCppFingerprint -Root $Repo
    $exitCode = 0

    foreach ($config in $Configs) {
        $recordProp = $stamp.records.PSObject.Properties[$config]
        if (-not $recordProp) {
            Write-Line ("[STALE] {0}: no build record (the last build used different targets)" -f $config) "Yellow"
            $exitCode = 1
            continue
        }
        $record = $recordProp.Value
        $problems = New-Object System.Collections.Generic.List[string]

        foreach ($prop in $record.inputs.PSObject.Properties) {
            $rel = [string]$prop.Name
            if (-not $current.Contains($rel)) {
                $problems.Add("missing input: $rel")
            }
            elseif ([string]$current[$rel] -ne [string]$prop.Value) {
                $problems.Add("changed input: $rel")
            }
        }
        foreach ($rel in @($current.Keys)) {
            if (-not $record.inputs.PSObject.Properties[$rel]) {
                $problems.Add("new input file: $rel")
            }
        }

        $artifactLines = New-Object System.Collections.Generic.List[string]
        foreach ($prop in $record.outputs.PSObject.Properties) {
            $name = [string]$prop.Name
            $abs = Join-Path $BinDir $name
            if (-not (Test-Path -LiteralPath $abs)) {
                $problems.Add("missing artifact: $name")
                continue
            }
            $currentHash = Get-FileSha256 -Path $abs
            if ($currentHash -ne [string]$prop.Value) {
                $problems.Add("artifact replaced on disk: $name")
            }
            $size = (Get-Item -LiteralPath $abs).Length
            $hashPrefix = ([string]$prop.Value)
            if ($hashPrefix.Length -gt 12) {
                $hashPrefix = $hashPrefix.Substring(0, 12)
            }
            $artifactLines.Add(("{0}  sha256 {1}...  {2:N0} bytes" -f $name, $hashPrefix, $size))
        }

        if ($record.godot_cpp) {
            $recordHead = [string]$record.godot_cpp.head
            $recordDirty = [string]$record.godot_cpp.dirty
            if ($recordHead -ne [string]$godotCpp.head) {
                $currentHead = [string]$godotCpp.head
                if (-not $currentHead) {
                    $currentHead = "<unavailable>"
                }
                $problems.Add("godot-cpp checkout changed: $currentHead")
            }
            elseif ($recordDirty -ne [string]$godotCpp.dirty) {
                $problems.Add("godot-cpp working tree changed since the build")
            }
        }

        if ($problems.Count -eq 0) {
            $builtAt = [string]$record.built_at
            try {
                $builtAt = ([datetime]$record.built_at).ToString("yyyy-MM-dd HH:mm:ss")
            }
            catch {
                $null = $_
            }
            Write-Line ("[FRESH] {0} is up to date - built {1}" -f $config, $builtAt) "Green"
            foreach ($line in $artifactLines) {
                Write-Line ("        artifact: {0}" -f $line) "DarkGray"
            }
            Write-Line ("        inputs  : {0} file(s) unchanged, godot-cpp {1}" -f $current.Count, $godotCpp.head) "DarkGray"
            if ($record.encryption -and [bool]$record.encryption.enabled) {
                Write-Line ("        key id  : {0}" -f $record.encryption.key_id) "DarkGray"
            }
            else {
                Write-Line "        key id  : keyless (encryption disabled)" "DarkGray"
            }
            if ([bool]$record.tests -and -not $TestsRequested) {
                Write-Line "        note    : last debug build included tests; test-library freshness is not tracked" "DarkGray"
            }
        }
        else {
            Write-Line ("[STALE] {0} needs a rebuild - {1} difference(s)" -f $config, $problems.Count) "Yellow"
            $shown = 0
            foreach ($problem in $problems) {
                if ($shown -ge 25) {
                    Write-Line ("        ... and {0} more" -f ($problems.Count - 25)) "DarkGray"
                    break
                }
                Write-Line ("        {0}" -f $problem) "Gray"
                $shown++
            }
            $testArg = ""
            if ($TestsRequested) {
                $testArg = " -IncludeTests"
            }
            Write-Line ("        rebuild: .vscode\build_remote.ps1 -Target {0}{1}" -f $config, $testArg) "Cyan"
            $exitCode = 1
        }
    }

    return $exitCode
}

# --- Entry point -------------------------------------------------------------
if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}
if (-not $StampPath) {
    $StampPath = Join-Path $RepoRoot ".cache\build_freshness.json"
}
$StampPath = [System.IO.Path]::GetFullPath($StampPath)
$BinDir = Join-Path $RepoRoot $AddonBinRel

$configs = @($Target)
if ($Target -eq "template_debug_and_release" -or $Target -eq "all") {
    $configs = @("template_release", "template_debug")
}

try {
    if ($Record) {
        Invoke-Record -Repo $RepoRoot -BinDir $BinDir -Path $StampPath -Configs $configs -TestsRequested ([bool]$IncludeTests)
        exit 0
    }
    exit (Invoke-Check -Repo $RepoRoot -BinDir $BinDir -Path $StampPath -Configs $configs -TestsRequested ([bool]$IncludeTests))
}
catch {
    Write-Line ("[freshness] check failed: {0}" -f $_.Exception.Message) "Red"
    Write-Line "        Treat this as STALE (exit 2)." "DarkGray"
    exit 2
}
