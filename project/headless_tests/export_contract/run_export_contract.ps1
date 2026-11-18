#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$WorkRoot = '',
    [string]$KeyedTemplate = '',
    [string]$KeyFile = '',
    [string[]]$Only = @(),
    [int]$ExportTimeoutSec = 420,
    [int]$BootTimeoutSec = 90,
    [switch]$NegativeControl,
    # Fail the run when a case was skipped for a real reason (missing template, fixture or
    # prerequisite stage). Contract limitations are reported separately and do not trip it.
    [switch]$RequireFullCoverage,
    # Remove the work root after a passing run; a failing run keeps its artefacts for diagnosis.
    [switch]$Cleanup
)

# Export-contract gate.
#
# Proves the archive/bundle contract on real exports, not on unit fixtures:
#
#   * protected and unprotected success, bundling off
#   * the fail-closed paths (empty PCK key, include filter miss, exclude filter)
#   * subset exports (scenes, resources, exclude, dedicated server)
#   * partial salvage and a stale archive left by a failed export
#   * the pre-flight rewrite estimate (previous exe, PCK, console wrapper and archive)
#   * fail-closed when any EXPORT_MESSAGE_ERROR reaches _export_end (injected by the
#     sibling fixture plugin through the same platform message list the engine uses)
#   * encrypt_directory on and off
#   * a corrupted TOC
#   * a plaintext archive substituted into a protected build (the archive id is
#     copied out of the cleartext header) must be refused at boot, including when
#     an override.cfg beside the executable flips dsgd_extension/protected to false
#   * "a skipped payload implies a declared archive" with failures injected at
#     create(), the first add_texture(), mid-walk and finalize()
#   * the pack scan: validity, key-leak absence, and a positive control that a
#     key written in the clear is found
#   * magic parity between the C++, GDScript and scanner derivations
#
# Every boot runs from a staged copy of the release directory, sets
# DSPKG_BOOT_VERBOSE=1 and must print exactly one `DSPKG_BOOT status=ok` line
# (stdout and the DSPKG_VALIDATE_OUT sink). Absence, a timeout or a duplicate is
# a failure. A deliberately broken export fails the gate: the fail-closed cases
# assert that no `status=ok` line exists and that the scanner rejects the pack.
#
# Requirements: the Godot console binary named by .env or by the environment
# (GODOT_CONSOLE_PATH), a 4.7.2 keyed release template plus its key file
# (protected cases), and the extension DLLs in
# project/addons/directstorage_accelerator/bin/. The positive-control fixture is
# in this directory; the old .agents/scratch copy is still accepted as a fallback.
# The key is read from the template directory at run time and never written into
# this script or the fixture.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$WorkspaceRoot = $Here
while (-not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot 'SConstruct'))) {
    $Parent = Split-Path -Parent $WorkspaceRoot
    if ([string]::IsNullOrEmpty($Parent) -or $Parent -eq $WorkspaceRoot) { throw 'Workspace root not found.' }
    $WorkspaceRoot = $Parent
}

$FixtureRoot = Join-Path $Here 'fixture'
$CaseScripts = Join-Path $Here 'cases'
$ScannerScript = Join-Path $WorkspaceRoot 'project\addons\directstorage_accelerator\tools\dspkg_pack_scan.gd'
$LiveAddon = Join-Path $WorkspaceRoot 'project\addons\directstorage_accelerator'
# The positive control lives in the repository so a fresh clone can run it; the old
# gitignored scratch copy is still honoured for workstations that already have one.
$ControlFixture = Join-Path $Here 'control_fixture\key_scan_control'
if (-not (Test-Path -LiteralPath $ControlFixture)) {
    $ControlFixture = Join-Path $WorkspaceRoot '.agents\scratch\key_scan_control'
}
$ControlKey = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

if ([string]::IsNullOrEmpty($WorkRoot)) {
    $WorkRoot = Join-Path $Here ('out\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
$WorkRoot = (Resolve-Path -LiteralPath $WorkRoot).Path
$LogDir = Join-Path $WorkRoot 'logs'
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

# ---------------------------------------------------------------- environment

$Godot = ''
$EnvFile = Join-Path $WorkspaceRoot '.env'
if (Test-Path -LiteralPath $EnvFile) {
    $line = Get-Content -LiteralPath $EnvFile | Where-Object { $_ -match '^\s*GODOT_CONSOLE_PATH\s*=' } | Select-Object -First 1
    if ($null -ne $line) { $Godot = ($line -split '=', 2)[1].Trim().Trim('"').Trim("'") }
}
if ([string]::IsNullOrEmpty($Godot)) {
    # A worktree, a script or a second machine can supply the binary without writing .env.
    $Godot = [string]$env:GODOT_CONSOLE_PATH
    if (-not [string]::IsNullOrEmpty($Godot)) { $Godot = $Godot.Trim().Trim('"').Trim("'") }
}
if ([string]::IsNullOrEmpty($Godot) -or -not (Test-Path -LiteralPath $Godot)) {
    throw 'Godot console binary not found: set GODOT_CONSOLE_PATH in .env or in the environment.'
}
if ($Godot -notmatch 'console') {
    throw ('GODOT_CONSOLE_PATH must name a console build (stdout streaming); got: ' + $Godot)
}
if (-not (Test-Path -LiteralPath $ScannerScript)) { throw ('Pack scanner missing: ' + $ScannerScript) }

if ([string]::IsNullOrEmpty($KeyedTemplate)) {
    $KeyedTemplate = Join-Path $WorkspaceRoot '.agents\scratch\test_keyed_template_release\godot.windows.template_release.x86_64.exe'
}
if ([string]::IsNullOrEmpty($KeyFile)) {
    $KeyFile = Join-Path $WorkspaceRoot '.agents\scratch\test_keyed_template_release\key.txt'
}
$TemplatePath = ''
$TemplateKey = ''
$TemplateReason = ''
if (-not (Test-Path -LiteralPath $KeyedTemplate)) {
    $TemplateReason = 'keyed release template missing: ' + $KeyedTemplate
} elseif (-not (Test-Path -LiteralPath $KeyFile)) {
    $TemplateReason = 'template key file missing: ' + $KeyFile
} else {
    $TemplateKey = (Get-Content -LiteralPath $KeyFile -Raw).Trim().ToLowerInvariant()
    if ($TemplateKey -notmatch '^[0-9a-f]{64}$') {
        $TemplateReason = 'template key is not 64 hex characters'
        $TemplateKey = ''
    } else {
        $TemplatePath = (Resolve-Path -LiteralPath $KeyedTemplate).Path.Replace('\', '/')
    }
}
$ProtectedAvailable = -not [string]::IsNullOrEmpty($TemplatePath)

Write-Output ('[gate] workspace=' + $WorkspaceRoot)
Write-Output ('[gate] godot=' + $Godot)
Write-Output ('[gate] work=' + $WorkRoot)
if ($ProtectedAvailable) {
    Write-Output ('[gate] keyed_template=' + $TemplatePath)
} else {
    Write-Output ('[gate] keyed_template=unavailable (' + $TemplateReason + ')')
}

$script:Results = New-Object System.Collections.Generic.List[object]
$script:Covered = New-Object System.Collections.Generic.List[string]
$script:NotCovered = New-Object System.Collections.Generic.List[string]
$script:KnownLimits = New-Object System.Collections.Generic.List[string]
$script:CurrentCase = ''
$script:Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { -not [string]::IsNullOrEmpty($_) })
$script:LastBoot = $null
$script:ProtectedStage = $null
$script:ProtectedArchive = $null
$script:PlainArchiveStage = $null
$script:BootSeq = 0
$MasterProject = ''

# ---------------------------------------------------------------- reporting

function Add-Check {
    param([string]$Name, [bool]$Pass, [string]$Detail)
    $script:Results.Add([pscustomobject]@{ Case = $script:CurrentCase; Name = $Name; Pass = $Pass; Detail = $Detail })
    $tag = if ($Pass) { 'PASS' } else { 'FAIL' }
    $text = ($Detail -replace "`r?`n", ' | ').Trim()
    if ($text.Length -gt 400) { $text = $text.Substring(0, 400) + '...' }
    Write-Output ('[{0}] {1} :: {2}' -f $tag, $Name, $text)
}

function Add-NotCovered {
    param([string]$Name, [string]$Reason)
    $script:Covered.Remove($Name) | Out-Null
    $script:NotCovered.Add($Name + ' (' + $Reason + ')')
    Write-Output ('[SKIP] ' + $Name + ' :: ' + $Reason)
}

# A contract limitation is a sub-check that cannot pass on any machine (an engine behaviour or
# an unreachable branch), so it is reported separately from a real coverage gap and the case it
# belongs to stays in `covers=`. -RequireFullCoverage only trips on real gaps.
function Add-KnownLimitation {
    param([string]$Name, [string]$Reason)
    $script:KnownLimits.Add($Name + ' (' + $Reason + ')')
    Write-Output ('[LIMIT] ' + $Name + ' :: ' + $Reason)
}

function Invoke-Case {
    param([string]$Name, [scriptblock]$Body)
    if ($script:Only.Count -gt 0 -and -not ($script:Only -contains $Name)) { return }
    $script:CurrentCase = $Name
    $script:Covered.Add($Name)
    Write-Output ''
    Write-Output ('[case] ' + $Name)
    & $Body
}

function Save-Log {
    param([string]$Name, [string]$Text)
    $path = Join-Path $LogDir ($Name + '.log')
    [System.IO.File]::WriteAllText($path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

# ---------------------------------------------------------------- process I/O

function Stop-ProcessTree {
    param([int]$ProcessId)
    $children = @(Get-CimInstance Win32_Process -Filter ('ParentProcessId=' + $ProcessId) -ErrorAction SilentlyContinue)
    foreach ($child in $children) { Stop-ProcessTree -ProcessId ([int]$child.ProcessId) }
    try { Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue } catch { Write-Verbose ('process already gone: ' + $_.Exception.Message) }
}

function Invoke-Tool {
    param(
        [string]$Exe,
        [string]$Arguments,
        [string]$WorkingDirectory,
        [int]$TimeoutSec,
        [hashtable]$Environment = @{}
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $Arguments
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($key in $Environment.Keys) { $psi.EnvironmentVariables[$key] = [string]$Environment[$key] }

    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $timedOut = $false
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        $timedOut = $true
        Stop-ProcessTree -ProcessId $proc.Id
        try { $proc.WaitForExit(5000) | Out-Null } catch { Write-Verbose ('exit wait interrupted: ' + $_.Exception.Message) }
    }
    $out = ''
    $err = ''
    try { $out = [string]$outTask.Result } catch { $out = '' }
    try { $err = [string]$errTask.Result } catch { $err = '' }
    $code = -1
    if (-not $timedOut) { $code = $proc.ExitCode }
    return [pscustomobject]@{ ExitCode = $code; TimedOut = $timedOut; Out = $out; Err = $err }
}

# ---------------------------------------------------------------- project setup

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrEmpty($parent) -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function New-ProjectCopy {
    param([string]$Name)
    $dir = Join-Path $WorkRoot ('case-' + $Name)
    if (Test-Path -LiteralPath $dir) { throw ('case directory already exists: ' + $dir) }
    Copy-Item -LiteralPath $MasterProject -Destination $dir -Recurse -Force
    return $dir
}

function Write-Preset {
    param(
        [string]$ProjectDir,
        [string]$Name,
        [string]$Filter = 'all_resources',
        [string]$Files = 'PackedStringArray()',
        [bool]$Dedicated = $false,
        [bool]$Runnable = $false,
        [string]$Template = '',
        [string]$EncryptInclude = '',
        [string]$EncryptExclude = '',
        [bool]$EncryptPck = $true,
        [bool]$EncryptDirectory = $false,
        [bool]$Bundle = $true,
        [bool]$Pairing = $true,
        [string]$Customized = '{}',
        [string]$VerifyPack = ''
    )
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('[runnable_presets]')
    $lines.Add('')
    $lines.Add('[preset.0]')
    $lines.Add('name="' + $Name + '"')
    $lines.Add('platform="Windows Desktop"')
    $lines.Add('runnable=' + $Runnable.ToString().ToLowerInvariant())
    $lines.Add('dedicated_server=' + $Dedicated.ToString().ToLowerInvariant())
    $lines.Add('custom_features=""')
    $lines.Add('export_filter="' + $Filter + '"')
    $lines.Add('export_files=' + $Files)
    if ($Filter -eq 'customized') { $lines.Add('customized_files=' + $Customized) }
    $lines.Add('include_filter=""')
    $lines.Add('exclude_filter=""')
    $lines.Add('export_path="' + $Name + '.pck"')
    $lines.Add('patches=PackedStringArray()')
    $lines.Add('patch_delta_encoding=false')
    $lines.Add('encryption_include_filters="' + $EncryptInclude + '"')
    $lines.Add('encryption_exclude_filters="' + $EncryptExclude + '"')
    $lines.Add('seed=0')
    $lines.Add('encrypt_pck=' + $EncryptPck.ToString().ToLowerInvariant())
    $lines.Add('encrypt_directory=' + $EncryptDirectory.ToString().ToLowerInvariant())
    $lines.Add('script_export_mode=0')
    $lines.Add('')
    $lines.Add('[preset.0.options]')
    $lines.Add('custom_template/debug=""')
    $lines.Add('custom_template/release="' + $Template + '"')
    $lines.Add('binary_format/embed_pck=false')
    $lines.Add('binary_format/architecture="x86_64"')
    $lines.Add('application/modify_resources=false')
    $lines.Add('dsgd_extension/bundle_textures_dspkg=' + $Bundle.ToString().ToLowerInvariant())
    if (-not [string]::IsNullOrEmpty($VerifyPack)) {
        $lines.Add('dsgd_extension/verify_pack_on_export=' + $VerifyPack)
    }
    $lines.Add('dsgd_extension/enforce_archive_pairing=' + $Pairing.ToString().ToLowerInvariant())
    $lines.Add('dsgd_extension/steampipe_1mb_alignment=false')
    Write-Utf8NoBom -Path (Join-Path $ProjectDir 'export_presets.cfg') -Text (($lines -join "`r`n") + "`r`n")
}

function Write-CredentialFile {
    param([string]$ProjectDir, [string]$KeyHex)
    $path = Join-Path $ProjectDir '.godot\export_credentials.cfg'
    if ([string]::IsNullOrEmpty($KeyHex)) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        return
    }
    Write-Utf8NoBom -Path $path -Text ("[preset.0]`r`nscript_encryption_key=`"" + $KeyHex + "`"`r`n")
}

# ---------------------------------------------------------------- export

function Export-Release {
    param([string]$ProjectDir, [string]$Preset, [string]$OutputDir, [hashtable]$Environment = @{})
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $exe = Join-Path $OutputDir 'game.exe'
    $arguments = '--headless --path "' + $ProjectDir + '" --export-release "' + $Preset + '" "' + $exe + '"'
    return Invoke-Tool -Exe $Godot -Arguments $arguments -WorkingDirectory $ProjectDir -TimeoutSec $ExportTimeoutSec -Environment $Environment
}

function Export-Pack {
    param([string]$ProjectDir, [string]$Preset, [string]$OutputDir, [hashtable]$Environment = @{})
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $pck = Join-Path $OutputDir ($Preset + '.pck')
    $arguments = '--headless --path "' + $ProjectDir + '" --export-pack "' + $Preset + '" "' + $pck + '"'
    return Invoke-Tool -Exe $Godot -Arguments $arguments -WorkingDirectory $ProjectDir -TimeoutSec $ExportTimeoutSec -Environment $Environment
}

function Get-ExportText {
    param($Run)
    return ([string]$Run.Out + "`n" + [string]$Run.Err)
}

function Get-ArchiveList {
    param([string]$Directory)
    if (-not (Test-Path -LiteralPath $Directory)) { return @() }
    return @(Get-ChildItem -LiteralPath $Directory -Filter '*.dspkg' -File -ErrorAction SilentlyContinue)
}

function Copy-ReleaseStage {
    param([string]$SourceDir, [string]$Name)
    $stage = Join-Path $WorkRoot ('stage-' + $Name)
    if (Test-Path -LiteralPath $stage) { throw ('stage directory already exists: ' + $stage) }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    foreach ($item in @(Get-ChildItem -LiteralPath $SourceDir -File)) {
        $dest = Join-Path $stage $item.Name
        $linked = $false
        if ($item.Length -gt 1048576) {
            try {
                New-Item -ItemType HardLink -Path $dest -Target $item.FullName -ErrorAction Stop | Out-Null
                $linked = $true
            } catch { $linked = $false }
        }
        if (-not $linked) { Copy-Item -LiteralPath $item.FullName -Destination $dest -Force }
    }
    return $stage
}

# ---------------------------------------------------------------- verdict parsers

function Get-BootResult {
    param($Run, [string]$SinkPath)
    $combined = ([string]$Run.Out) + "`n" + ([string]$Run.Err)
    $okRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^DSPKG_BOOT status=ok mode=(protected|unprotected) entries=(\d+) key_id=(\S+)\s*$')
    $failRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^DSPKG_BOOT status=fail reason=(\w+) mode=(protected|unprotected)\s*$')
    $probeRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^DSPKG_PROBE display=(\S+) args=(\d+) user_args=\[(.*?)\] toc_key=(-?\d+) payload_used=(\w+) payload_unused=(\w+)\s*$')

    $ok = $okRegex.Matches($combined)
    $fail = $failRegex.Matches($combined)
    $probe = $probeRegex.Match($combined)

    $sinkText = ''
    if (-not [string]::IsNullOrEmpty($SinkPath) -and (Test-Path -LiteralPath $SinkPath)) {
        $sinkText = [System.IO.File]::ReadAllText($SinkPath)
    }
    $sinkOk = $okRegex.Matches($sinkText)
    $sinkFail = $failRegex.Matches($sinkText)

    $mode = ''
    $entries = -1
    $keyId = ''
    if ($ok.Count -gt 0) {
        $mode = $ok[0].Groups[1].Value
        $entries = [int]$ok[0].Groups[2].Value
        $keyId = $ok[0].Groups[3].Value
    }
    $reason = ''
    if ($fail.Count -gt 0) { $reason = $fail[0].Groups[1].Value }

    return [pscustomobject]@{
        TimedOut = $Run.TimedOut
        ExitCode = $Run.ExitCode
        OkCount = $ok.Count
        OkLine = if ($ok.Count -gt 0) { $ok[0].Value.Trim() } else { '' }
        OkMode = $mode
        OkEntries = $entries
        OkKeyId = $keyId
        SinkOkCount = $sinkOk.Count
        SinkFailCount = $sinkFail.Count
        FailCount = $fail.Count
        FailReason = $reason
        FailLine = if ($fail.Count -gt 0) { $fail[0].Value.Trim() } else { '' }
        ProbeDisplay = if ($probe.Success) { $probe.Groups[1].Value } else { '' }
        ProbeArgs = if ($probe.Success) { [int]$probe.Groups[2].Value } else { -1 }
        ProbeUserArgs = if ($probe.Success) { $probe.Groups[3].Value } else { '' }
        ProbeKeySize = if ($probe.Success) { [int]$probe.Groups[4].Value } else { -2 }
        ProbePayloadUsed = if ($probe.Success) { $probe.Groups[5].Value } else { '' }
        ProbePayloadUnused = if ($probe.Success) { $probe.Groups[6].Value } else { '' }
        Combined = $combined
    }
}

function Invoke-Boot {
    param([string]$StageDir, [string]$UserArgs = '', [int]$Timeout = 0, [string]$ExeName = 'game.exe')
    $exe = Join-Path $StageDir $ExeName
    if (-not (Test-Path -LiteralPath $exe)) {
        return [pscustomobject]@{ TimedOut = $false; ExitCode = -1; MissingExecutable = $true; Boot = $null }
    }
    $sink = Join-Path $StageDir 'sentinel.txt'
    if (Test-Path -LiteralPath $sink) { Remove-Item -LiteralPath $sink -Force }
    $arguments = '--headless'
    if (-not [string]::IsNullOrEmpty($UserArgs)) { $arguments += ' -- ' + $UserArgs }
    $timeout = $Timeout
    if ($timeout -le 0) { $timeout = $BootTimeoutSec }
    $run = Invoke-Tool -Exe $exe -Arguments $arguments -WorkingDirectory $StageDir -TimeoutSec $timeout -Environment @{
        DSPKG_BOOT_VERBOSE = '1'
        DSPKG_VALIDATE_OUT = $sink
    }
    $boot = Get-BootResult -Run $run -SinkPath $sink
    $script:BootSeq = $script:BootSeq + 1
    $bootLog = Join-Path $LogDir ($script:CurrentCase + '.boot' + $script:BootSeq + '.log')
    $sinkText = ''
    if (Test-Path -LiteralPath $sink) { $sinkText = [System.IO.File]::ReadAllText($sink) }
    $bootText = ('arguments: --headless' + $(if ([string]::IsNullOrEmpty($UserArgs)) { '' } else { ' -- ' + $UserArgs }) + "`r`n`r`n--- stdout ---`r`n" + [string]$run.Out + "`r`n--- stderr ---`r`n" + [string]$run.Err + "`r`n--- sink ---`r`n" + $sinkText)
    [System.IO.File]::WriteAllText($bootLog, $bootText, (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{ TimedOut = $run.TimedOut; ExitCode = $run.ExitCode; MissingExecutable = $false; Boot = $boot }
}

function Assert-BootOk {
    param([string]$Case, [string]$StageDir, [string]$Mode, [int]$MinEntries, [string]$UserArgs = '')
    $script:LastBoot = $null
    $result = Invoke-Boot -StageDir $StageDir -UserArgs $UserArgs
    if ($result.MissingExecutable) {
        Add-Check ($Case + '.boot_executable') $false 'no game.exe in the staged release directory'
        return
    }
    $boot = $result.Boot
    $script:LastBoot = $boot
    Add-Check ($Case + '.boot_no_timeout') (-not $boot.TimedOut) ('timed_out=' + $boot.TimedOut + ' exit=' + $boot.ExitCode)
    Add-Check ($Case + '.boot_single_ok') (($boot.OkCount -eq 1) -and ($boot.SinkOkCount -eq 1)) ('stdout_ok=' + $boot.OkCount + ' sink_ok=' + $boot.SinkOkCount + ' line=' + $boot.OkLine)
    Add-Check ($Case + '.boot_mode') ($boot.OkMode -eq $Mode) ('mode=' + $boot.OkMode + ' expected=' + $Mode)
    Add-Check ($Case + '.boot_entries') (($boot.OkEntries -ge $MinEntries) -and ($boot.OkEntries -ge 0)) ('entries=' + $boot.OkEntries + ' min=' + $MinEntries)
    Add-Check ($Case + '.boot_no_failure_line') ($boot.FailCount -eq 0) ('fail=' + $boot.FailLine)
}

function Assert-BootFail {
    param([string]$Case, [string]$StageDir, [string]$Reason, [string]$UserArgs = '--dspkg-validate=protected', [int]$Timeout = 0, [switch]$RequireNonZeroExit)
    $script:LastBoot = $null
    $result = Invoke-Boot -StageDir $StageDir -UserArgs $UserArgs -Timeout $Timeout
    if ($result.MissingExecutable) {
        Add-Check ($Case + '.boot_absent_no_executable') $true 'no executable was written; absence is the failure signal'
        return
    }
    $boot = $result.Boot
    $script:LastBoot = $boot
    Add-Check ($Case + '.boot_no_timeout') (-not $boot.TimedOut) ('timed_out=' + $boot.TimedOut + ' exit=' + $result.ExitCode)
    Add-Check ($Case + '.boot_failure_reason') (($boot.FailCount -eq 1) -and ($boot.FailReason -eq $Reason)) ('reason=' + $boot.FailReason + ' expected=' + $Reason + ' line=' + $boot.FailLine)
    Add-Check ($Case + '.boot_no_ok_line') (($boot.OkCount -eq 0) -and ($boot.SinkOkCount -eq 0)) ('stdout_ok=' + $boot.OkCount + ' sink_ok=' + $boot.SinkOkCount)
    # The verdict is the sentinel line, but a launcher that only reads the process
    # code must still see a failure. A failure verdict can be overwritten by a
    # later quit(0) from the game's own main loop, so this is opt-in and asserted
    # where that specific overwrite is expected to happen.
    if ($RequireNonZeroExit) {
        Add-Check ($Case + '.boot_nonzero_exit') ($result.ExitCode -ne 0) ('exit=' + $result.ExitCode)
    }
}

$verifyRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^\[DirectStorage Exporter\] PACK_VERIFY status=(\w+) reason=(\w+) dir_encrypted=(\w+) entries=(\d+) bytes=(\d+) ms=(\d+) file=(.+)$')

function Invoke-Scan {
    param([string]$PackPath)
    $arguments = '--headless --path "' + $MasterProject + '" --script "' + $ScannerScript + '" -- --pack "' + $PackPath + '"'
    $run = Invoke-Tool -Exe $Godot -Arguments $arguments -WorkingDirectory $MasterProject -TimeoutSec 300
    $text = Get-ExportText $run
    $okRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^DSPKG_SCAN status=ok entries=(\d+) file=(.*)$')
    $failRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^DSPKG_SCAN status=fail reason=(\w+) file=(.*)$')
    $ok = $okRegex.Match($text)
    $fail = $failRegex.Match($text)
    return [pscustomobject]@{
        TimedOut = $run.TimedOut
        ExitCode = $run.ExitCode
        Ok = $ok.Success
        Entries = if ($ok.Success) { [int]$ok.Groups[1].Value } else { -1 }
        Reason = if ($fail.Success) { $fail.Groups[1].Value } else { '' }
        Verdict = if ($ok.Success) { $ok.Value.Trim() } elseif ($fail.Success) { $fail.Value.Trim() } else { 'no verdict' }
        Raw = $text
    }
}

function Invoke-PackProbe {
    param([string]$PackPath)
    # A real Godot project in the working directory makes res:// resolve to that
    # project's files instead of the mounted pack, so run from a neutral folder.
    $arguments = '--headless --main-pack "' + $PackPath + '" --script "' + (Join-Path $CaseScripts 'pack_probe.gd') + '"'
    $run = Invoke-Tool -Exe $Godot -Arguments $arguments -WorkingDirectory $NeutralDir -TimeoutSec 120
    $text = Get-ExportText $run
    $publishedRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^PACK_PROBE published bundle=(\w+) protected=(\w+) archive_id=(-?\d+) archive_name=(\S+)\s*$')
    $payloadRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^PACK_PROBE payload used=(\w+) unused=(\w+) trigger=(\w+)\s*$')
    $scriptsRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^PACK_PROBE scripts gd=(\d+) gdc=(\d+) unreadable=(\d+) magic_hits=(\d+)\s*$')
    $published = $publishedRegex.Match($text)
    $payload = $payloadRegex.Match($text)
    $scripts = $scriptsRegex.Match($text)
    return [pscustomobject]@{
        TimedOut = $run.TimedOut
        Ran = $published.Success
        Bundle = if ($published.Success) { $published.Groups[1].Value -eq 'true' } else { $false }
        Protected = if ($published.Success) { $published.Groups[2].Value -eq 'true' } else { $false }
        ArchiveId = if ($published.Success) { [int64]$published.Groups[3].Value } else { [int64]0 }
        ArchiveName = if ($published.Success) { $published.Groups[4].Value } else { '' }
        PayloadUsed = if ($payload.Success) { $payload.Groups[1].Value -eq 'true' } else { $null }
        PayloadUnused = if ($payload.Success) { $payload.Groups[2].Value -eq 'true' } else { $null }
        PayloadTrigger = if ($payload.Success) { $payload.Groups[3].Value -eq 'true' } else { $null }
        ScriptsScanned = if ($scripts.Success) { [int]$scripts.Groups[1].Value + [int]$scripts.Groups[2].Value } else { -1 }
        MagicHits = if ($scripts.Success) { [int]$scripts.Groups[4].Value } else { -1 }
        UnreadableScripts = if ($scripts.Success) { [int]$scripts.Groups[3].Value } else { -1 }
        Raw = $text
    }
}

function Invoke-ArchiveProbe {
    param([string]$ProjectDir, [string]$ArchivePath, [int64]$ExpectedId)
    $arguments = '--headless --path "' + $ProjectDir + '" --script "' + (Join-Path $CaseScripts 'archive_probe.gd') + '"'
    $run = Invoke-Tool -Exe $Godot -Arguments $arguments -WorkingDirectory $ProjectDir -TimeoutSec 120 -Environment @{
        DSPKG_CONTRACT_ARCHIVE = $ArchivePath
        DSPKG_CONTRACT_EXPECTED_ID = $ExpectedId.ToString()
    }
    $text = Get-ExportText $run
    $headRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^ARCHIVE_PROBE open_err=(-?\d+) expected_id=(-?\d+) entries=(\d+)\s*$')
    $entryRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^ARCHIVE_PROBE entry=(.+)$')
    $head = $headRegex.Match($text)
    $entries = New-Object System.Collections.Generic.List[string]
    foreach ($match in $entryRegex.Matches($text)) {
        $entries.Add($match.Groups[1].Value.Trim())
    }
    return [pscustomobject]@{
        Ran = $head.Success
        TimedOut = $run.TimedOut
        OpenErr = if ($head.Success) { [int]$head.Groups[1].Value } else { -1 }
        Entries = if ($head.Success) { [int]$head.Groups[3].Value } else { -1 }
        EntryPaths = $entries
        Raw = $text
    }
}

# ---------------------------------------------------------------- master project

$MasterProject = Join-Path $WorkRoot '_master'
Copy-Item -LiteralPath $FixtureRoot -Destination $MasterProject -Recurse -Force
$addonTarget = Join-Path $MasterProject 'addons\directstorage_accelerator'
if (Test-Path -LiteralPath $addonTarget) { throw ('fixture already carries an addon copy: ' + $addonTarget) }
Copy-Item -LiteralPath $LiveAddon -Destination $addonTarget -Recurse -Force
# The export copies only the DLLs the .gdextension names, so the linker and symbol
# artefacts in bin/ (a 41 MB pdb and a 27 MB ilk on a debug-symbols build) would
# otherwise be duplicated into every case directory for nothing.
Get-ChildItem -LiteralPath (Join-Path $addonTarget 'bin') -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.pdb', '.ilk', '.lib', '.exp' } |
    Remove-Item -Force

$fixtureJson = Get-Content -LiteralPath (Join-Path $MasterProject 'data\dstex_fixtures.json') -Raw | ConvertFrom-Json
$fixtureMap = @{ used = 'used.dstex'; unused = 'unused.dstex'; trigger = 'zzz_trigger.dstex' }
# The fixture's assets/ directory is empty in the repository, so git does not track it and a fresh
# clone has no such directory: create it before the decoded payloads are written into the copy.
$assetsTarget = Join-Path $MasterProject 'assets'
if (-not (Test-Path -LiteralPath $assetsTarget)) {
    New-Item -ItemType Directory -Path $assetsTarget -Force | Out-Null
}
foreach ($name in @('used', 'unused', 'trigger')) {
    $bytes = [Convert]::FromBase64String([string]$fixtureJson.$name)
    [System.IO.File]::WriteAllBytes((Join-Path $MasterProject ('assets\' + $fixtureMap[$name])), $bytes)
}

$NeutralDir = Join-Path $WorkRoot '_probe'
New-Item -ItemType Directory -Path $NeutralDir -Force | Out-Null

Write-Output '[gate] importing the fixture project'
$importRun = Invoke-Tool -Exe $Godot -Arguments ('--headless --path "' + $MasterProject + '" --import --quit') -WorkingDirectory $MasterProject -TimeoutSec 300
Save-Log -Name '_import' -Text (Get-ExportText $importRun)
if ($importRun.TimedOut) { throw 'fixture import timed out' }

# ---------------------------------------------------------------- protected success

Invoke-Case 'protected_success' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'protected_success' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'protected_success'
        Write-Preset -ProjectDir $proj -Name 'contract_protected' -Runnable $true -Template $TemplatePath -EncryptInclude '.dspkg/*' -EncryptPck $true -EncryptDirectory $false -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_protected' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'protected_success' -Text $log
        Add-Check 'protected_success.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
        Add-Check 'protected_success.deployed' ($log -match 'Successfully deployed standalone textures archive') 'archive must be deployed'
        Add-Check 'protected_success.writer_round_trip' ($log -match 'Writer round-trip verified') 'the written archive must open with the export key and id'
        $skipped = [regex]::Matches($log, 'Skipped texture payload from PCK').Count
        Add-Check 'protected_success.payloads_skipped' ($skipped -ge 3) ('skipped=' + $skipped)
        $archives = @(Get-ArchiveList $out)
        Add-Check 'protected_success.archive_present' ($archives.Count -eq 1) ('archives=' + $archives.Count)

        $stage = Copy-ReleaseStage -SourceDir $out -Name 'protected_success'
        Assert-BootOk -Case 'protected_success' -StageDir $stage -Mode 'protected' -MinEntries 3 -UserArgs '--dspkg-validate=protected'
        $boot = $script:LastBoot
        if ($null -ne $boot) {
            Add-Check 'protected_success.key_id_reported' (($boot.OkKeyId -ne 'none') -and -not [string]::IsNullOrEmpty($boot.OkKeyId)) ('key_id=' + $boot.OkKeyId)
            Add-Check 'protected_success.key_entry_48_bytes' ($boot.ProbeKeySize -eq 48) ('toc_key=' + $boot.ProbeKeySize)
            Add-Check 'protected_success.payload_absent_from_pck' ($boot.ProbePayloadUsed -eq 'false') ('payload_used=' + $boot.ProbePayloadUsed)
            Add-Check 'protected_success.validate_arg_reaches_player' ($boot.ProbeUserArgs -match '--dspkg-validate=protected') ('user_args=[' + $boot.ProbeUserArgs + ']')
            Add-Check 'protected_success.cmdline_args_empty' ($boot.ProbeArgs -eq 0) ('args=' + $boot.ProbeArgs)
            Add-Check 'protected_success.display_headless' ($boot.ProbeDisplay -eq 'headless') ('display=' + $boot.ProbeDisplay)
        }

        $scan = Invoke-Scan -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'protected_success.scan_ok' ($scan.Ok -and -not $scan.TimedOut) ('verdict=' + $scan.Verdict)
        $verify = $verifyRegex.Match($log)
        $verifyLine = (($log -split "`n" | Where-Object { $_ -match 'PACK_VERIFY' }) -join ' | ')
        Add-Check 'protected_success.pack_verify_line' ($verify.Success -and ($verify.Groups[1].Value -eq 'ok')) ('line=' + $verifyLine)
        Add-Check 'protected_success.pack_verify_matches_gate_scan' ($verify.Success -and $scan.Ok -and ([int]$verify.Groups[4].Value -eq $scan.Entries)) ('export_entries=' + $verify.Groups[4].Value + ' gate_entries=' + $scan.Entries)
        Add-Check 'protected_success.pack_verify_dir_not_encrypted' ($verify.Success -and ($verify.Groups[3].Value -eq 'false')) ('dir_encrypted=' + $verify.Groups[3].Value)
        $probe = Invoke-PackProbe -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'protected_success.pack_publishes_archive' ($probe.Bundle -and $probe.Protected) ('bundle=' + $probe.Bundle + ' protected=' + $probe.Protected)
        Add-Check 'protected_success.magic_absent_from_packed_scripts' (($probe.MagicHits -eq 0) -and ($probe.UnreadableScripts -eq 0) -and ($probe.ScriptsScanned -gt 0)) ('scripts=' + $probe.ScriptsScanned + ' unreadable=' + $probe.UnreadableScripts + ' magic_hits=' + $probe.MagicHits)
        $scanArchive = Invoke-Scan -PackPath (Join-Path $stage 'textures.dspkg')
        Add-Check 'protected_success.scan_distinguishes_archive' (-not $scanArchive.Ok) ('verdict=' + $scanArchive.Verdict)

        $script:ProtectedStage = $stage
        $script:ProtectedArchive = (Join-Path $stage 'textures.dspkg')
    }
}

# ---------------------------------------------------------------- encrypt_directory on

Invoke-Case 'protected_encrypt_directory_on' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'protected_encrypt_directory_on' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'protected_encrypt_directory_on'
        Write-Preset -ProjectDir $proj -Name 'contract_protected_edir' -Runnable $true -Template $TemplatePath -EncryptInclude '.dspkg/*' -EncryptPck $true -EncryptDirectory $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_protected_edir' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'protected_encrypt_directory_on' -Text $log
        Add-Check 'protected_encrypt_directory_on.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
        Add-Check 'protected_encrypt_directory_on.deployed' ($log -match 'Successfully deployed standalone textures archive') 'archive must be deployed'
        $archives = @(Get-ArchiveList $out)
        Add-Check 'protected_encrypt_directory_on.archive_present' ($archives.Count -eq 1) ('archives=' + $archives.Count)

        $stage = Copy-ReleaseStage -SourceDir $out -Name 'protected_encrypt_directory_on'
        Assert-BootOk -Case 'protected_encrypt_directory_on' -StageDir $stage -Mode 'protected' -MinEntries 3
        $boot = $script:LastBoot
        if ($null -ne $boot) {
            Add-Check 'protected_encrypt_directory_on.grammar_identical' ($boot.OkLine -match '^DSPKG_BOOT status=ok mode=protected entries=\d+ key_id=\S+$') ('line=' + $boot.OkLine)
            Add-Check 'protected_encrypt_directory_on.key_id_reported' ($boot.OkKeyId -ne 'none') ('key_id=' + $boot.OkKeyId)
            Add-Check 'protected_encrypt_directory_on.key_entry_48_bytes' ($boot.ProbeKeySize -eq 48) ('toc_key=' + $boot.ProbeKeySize)
        }
        $scan = Invoke-Scan -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'protected_encrypt_directory_on.scan_ok' ($scan.Ok -and -not $scan.TimedOut) ('verdict=' + $scan.Verdict)
        $verify = $verifyRegex.Match($log)
        $verifyLine = (($log -split "`n" | Where-Object { $_ -match 'PACK_VERIFY' }) -join ' | ')
        Add-Check 'protected_encrypt_directory_on.pack_verify_dir_encrypted' ($verify.Success -and ($verify.Groups[1].Value -eq 'ok') -and ($verify.Groups[3].Value -eq 'true')) ('line=' + $verifyLine)
    }
}

# ---------------------------------------------------------------- verify disabled

Invoke-Case 'verify_disabled' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'verify_disabled' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'verify_disabled'
        Write-Preset -ProjectDir $proj -Name 'contract_verify_disabled' -Runnable $true -Template $TemplatePath -EncryptInclude '.dspkg/*' -EncryptPck $true -EncryptDirectory $false -Bundle $true -Pairing $true -VerifyPack 'false'
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_verify_disabled' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'verify_disabled' -Text $log
        Add-Check 'verify_disabled.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
        Add-Check 'verify_disabled.deployed' ($log -match 'Successfully deployed standalone textures archive') 'archive must be deployed'
        $archives = @(Get-ArchiveList $out)
        Add-Check 'verify_disabled.archive_present' ($archives.Count -eq 1) ('archives=' + $archives.Count)
        $verify = $verifyRegex.Match($log)
        $verifyLine = (($log -split "`n" | Where-Object { $_ -match 'PACK_VERIFY' }) -join ' | ')
        Add-Check 'verify_disabled.skipped_line' ($verify.Success -and ($verify.Groups[1].Value -eq 'skipped') -and ($verify.Groups[2].Value -eq 'disabled_by_setting')) ('line=' + $verifyLine)
        Add-Check 'verify_disabled.no_ok_line' (-not ($log -match 'PACK_VERIFY status=ok')) ('line=' + $verifyLine)
    }
}

# ---------------------------------------------------------------- unprotected success

Invoke-Case 'unprotected_success' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'unprotected_success' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'unprotected_success'
        Write-Preset -ProjectDir $proj -Name 'contract_plaintext' -Runnable $true -Template $TemplatePath -EncryptPck $false -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex ''
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_plaintext' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'unprotected_success' -Text $log
        Add-Check 'unprotected_success.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
        Add-Check 'unprotected_success.deployed' ($log -match 'Successfully deployed standalone textures archive') 'archive must be deployed'
        $archives = @(Get-ArchiveList $out)
        Add-Check 'unprotected_success.archive_present' ($archives.Count -eq 1) ('archives=' + $archives.Count)

        $stage = Copy-ReleaseStage -SourceDir $out -Name 'unprotected_success'
        # Kept for protected_plaintext_archive: an ordinary unprotected export is
        # exactly the archive an attacker would substitute.
        $script:PlainArchiveStage = $stage
        Assert-BootOk -Case 'unprotected_success' -StageDir $stage -Mode 'unprotected' -MinEntries 3
        $boot = $script:LastBoot
        if ($null -ne $boot) {
            Add-Check 'unprotected_success.no_key_entry' ($boot.ProbeKeySize -eq -1) ('toc_key=' + $boot.ProbeKeySize)
            Add-Check 'unprotected_success.key_id_none' ($boot.OkKeyId -eq 'none') ('key_id=' + $boot.OkKeyId)
            Add-Check 'unprotected_success.payload_absent_from_pck' ($boot.ProbePayloadUsed -eq 'false') ('payload_used=' + $boot.ProbePayloadUsed)
            Add-Check 'unprotected_success.cmdline_args_empty' ($boot.ProbeArgs -eq 0) ('args=' + $boot.ProbeArgs)
        }
        $scan = Invoke-Scan -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'unprotected_success.scan_ok' ($scan.Ok -and -not $scan.TimedOut) ('verdict=' + $scan.Verdict)
    }
}

# ---------------------------------------------------------------- pre-flight rewrite estimate
#
# _warn_if_low_disk_space() sizes the artifact set this export rewrites from the
# files already at the target paths. Plant four known files at exactly those
# paths (the estimate is read before the old archive is removed), then assert the
# reported total. A fresh directory yields zero and would skip the new line.

Invoke-Case 'rewrite_estimate' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'rewrite_estimate' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'rewrite_estimate'
        Write-Preset -ProjectDir $proj -Name 'contract_rewrite_estimate' -Runnable $true -Template $TemplatePath -EncryptPck $false -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex ''
        $out = Join-Path $proj 'release'
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        # 10 MiB exactly: exe 1 + pck 2 + archive 3 + console wrapper 4.
        [System.IO.File]::WriteAllBytes((Join-Path $out 'game.exe'), (New-Object byte[] (1MB)))
        [System.IO.File]::WriteAllBytes((Join-Path $out 'game.pck'), (New-Object byte[] (2MB)))
        [System.IO.File]::WriteAllBytes((Join-Path $out 'textures.dspkg'), (New-Object byte[] (3MB)))
        [System.IO.File]::WriteAllBytes((Join-Path $out 'game.console.exe'), (New-Object byte[] (4MB)))
        $run = Export-Release -ProjectDir $proj -Preset 'contract_rewrite_estimate' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'rewrite_estimate' -Text $log
        Add-Check 'rewrite_estimate.reported' ($log -match 'Previous exe/PCK/archive in the target directory total 10\.0 MiB') 'the estimate must size exe + pck + console wrapper + previous archive'
        Add-Check 'rewrite_estimate.deployed' ($log -match 'Successfully deployed standalone textures archive') 'the export must still deploy its archive'
        Add-Check 'rewrite_estimate.archive_replaced' (@(Get-ArchiveList $out).Count -eq 1) ('archives=' + @(Get-ArchiveList $out).Count)
        Add-Check 'rewrite_estimate.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
    }
}

# ---------------------------------------------------------------- plaintext archive in a protected build
#
# The requirement must come from the build, not from the archive. A protected
# export ships an authenticated TOC, and the archive id it pairs against sits in
# the cleartext header, so an attacker who can replace the archive can also copy
# that id into a plaintext archive of their own. Before the build published the
# requirement, such an archive opened and the game booted. It must now be refused
# with the same reason the runtime uses for a decryption failure.

Invoke-Case 'protected_plaintext_archive' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'protected_plaintext_archive' $TemplateReason
    } elseif ($null -eq $script:ProtectedStage) {
        Add-NotCovered 'protected_plaintext_archive' 'no protected release to attack'
    } elseif ($null -eq $script:PlainArchiveStage) {
        Add-NotCovered 'protected_plaintext_archive' 'no unprotected release to source a plaintext archive from'
    } else {
        $stage = Join-Path $WorkRoot 'stage-protected_plaintext_archive'
        if (Test-Path -LiteralPath $stage) { throw ('stage directory already exists: ' + $stage) }
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        foreach ($item in @(Get-ChildItem -LiteralPath $script:ProtectedStage -File)) {
            Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $stage $item.Name) -Force
        }

        $protectedBytes = [System.IO.File]::ReadAllBytes((Join-Path $script:ProtectedStage 'textures.dspkg'))
        $plainBytes = [System.IO.File]::ReadAllBytes((Join-Path $script:PlainArchiveStage 'textures.dspkg'))

        # Fixture proof: the source archive really is a plaintext v4 archive and
        # the deployed one really is encrypted, or this case proves nothing.
        $plainVersion = [BitConverter]::ToUInt16($plainBytes, 4)
        $plainFlags = [BitConverter]::ToUInt16($plainBytes, 6)
        $plainKeyId = [BitConverter]::ToUInt64($plainBytes, 52)
        $protectedVersion = [BitConverter]::ToUInt16($protectedBytes, 4)
        $protectedFlags = [BitConverter]::ToUInt16($protectedBytes, 6)
        Add-Check 'protected_plaintext_archive.source_is_plaintext' (($plainVersion -eq 4) -and (($plainFlags -band 2) -eq 0) -and ($plainKeyId -eq 0)) ('version=' + $plainVersion + ' flags=' + $plainFlags + ' key_id=' + $plainKeyId)
        Add-Check 'protected_plaintext_archive.target_is_encrypted' (($protectedVersion -eq 4) -and (($protectedFlags -band 2) -ne 0)) ('version=' + $protectedVersion + ' flags=' + $protectedFlags)

        # The attack: copy the eight id bytes out of the cleartext protected header.
        $protectedId = [BitConverter]::ToUInt64($protectedBytes, 20)
        $plainId = [BitConverter]::ToUInt64($plainBytes, 20)
        [Array]::Copy([BitConverter]::GetBytes($protectedId), 0, $plainBytes, 20, 8)
        [System.IO.File]::WriteAllBytes((Join-Path $stage 'textures.dspkg'), $plainBytes)

        Add-Check 'protected_plaintext_archive.id_differs_before_copy' ($plainId -ne $protectedId) ('source_id=0x' + $plainId.ToString('x') + ' protected_id=0x' + $protectedId.ToString('x'))
        Add-Check 'protected_plaintext_archive.id_copied' ($protectedId -ne 0) ('id=0x' + $protectedId.ToString('x'))

        Assert-BootFail -Case 'protected_plaintext_archive' -StageDir $stage -Reason 'archive_unauthorized' -RequireNonZeroExit
    }
}

# ---------------------------------------------------------------- plaintext archive + override.cfg
#
# The requirement must not rest on a project setting that a file beside the executable
# can replace. Godot reads override.cfg from the exported binary's directory over
# project.binary, so this case plants exactly that file with
# dsgd_extension/protected=false and repeats the protected_plaintext_archive attack.
# The requirement has to survive on the key entry the encrypted PCK delivers. The control
# leg proves the override was applied (mode flips to unprotected) and that the honest
# encrypted archive still boots.

Invoke-Case 'protected_plaintext_archive_override' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'protected_plaintext_archive_override' $TemplateReason
    } elseif ($null -eq $script:ProtectedStage) {
        Add-NotCovered 'protected_plaintext_archive_override' 'no protected release to attack'
    } elseif ($null -eq $script:PlainArchiveStage) {
        Add-NotCovered 'protected_plaintext_archive_override' 'no unprotected release to source a plaintext archive from'
    } else {
        $stage = Join-Path $WorkRoot 'stage-protected_plaintext_archive_override'
        if (Test-Path -LiteralPath $stage) { throw ('stage directory already exists: ' + $stage) }
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        # Copy-Item, not Copy-ReleaseStage: that helper hardlinks files over 1 MiB and the
        # attack truncates textures.dspkg in place, which would corrupt the shared source.
        foreach ($item in @(Get-ChildItem -LiteralPath $script:ProtectedStage -File)) {
            Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $stage $item.Name) -Force
        }
        Write-Utf8NoBom -Path (Join-Path $stage 'override.cfg') -Text "dsgd_extension/protected=false`r`n"

        # Control leg: the override is honoured and the honest encrypted archive still boots.
        $control = Invoke-Boot -StageDir $stage
        if ($control.MissingExecutable) {
            Add-Check 'protected_plaintext_archive_override.control_executable' $false 'no game.exe in the staged release directory'
            return
        }
        Add-Check 'protected_plaintext_archive_override.control_no_timeout' (-not $control.Boot.TimedOut) ('timed_out=' + $control.Boot.TimedOut + ' exit=' + $control.ExitCode)
        Add-Check 'protected_plaintext_archive_override.control_single_ok' (($control.Boot.OkCount -eq 1) -and ($control.Boot.SinkOkCount -eq 1)) ('line=' + $control.Boot.OkLine)
        Add-Check 'protected_plaintext_archive_override.control_override_applied' ($control.Boot.OkMode -eq 'unprotected') ('mode=' + $control.Boot.OkMode)
        Add-Check 'protected_plaintext_archive_override.control_entries' ($control.Boot.OkEntries -ge 3) ('entries=' + $control.Boot.OkEntries)

        # Attack leg: the same substitution protected_plaintext_archive performs.
        $protectedBytes = [System.IO.File]::ReadAllBytes((Join-Path $script:ProtectedStage 'textures.dspkg'))
        $plainBytes = [System.IO.File]::ReadAllBytes((Join-Path $script:PlainArchiveStage 'textures.dspkg'))
        $plainVersion = [BitConverter]::ToUInt16($plainBytes, 4)
        $plainFlags = [BitConverter]::ToUInt16($plainBytes, 6)
        $plainKeyId = [BitConverter]::ToUInt64($plainBytes, 52)
        Add-Check 'protected_plaintext_archive_override.source_is_plaintext' (($plainVersion -eq 4) -and (($plainFlags -band 2) -eq 0) -and ($plainKeyId -eq 0)) ('version=' + $plainVersion + ' flags=' + $plainFlags + ' key_id=' + $plainKeyId)
        $protectedId = [BitConverter]::ToUInt64($protectedBytes, 20)
        [Array]::Copy([BitConverter]::GetBytes($protectedId), 0, $plainBytes, 20, 8)
        [System.IO.File]::WriteAllBytes((Join-Path $stage 'textures.dspkg'), $plainBytes)
        Add-Check 'protected_plaintext_archive_override.id_copied' ($protectedId -ne 0) ('id=0x' + $protectedId.ToString('x'))

        $attack = Invoke-Boot -StageDir $stage
        Add-Check 'protected_plaintext_archive_override.attack_no_timeout' (-not $attack.Boot.TimedOut) ('timed_out=' + $attack.Boot.TimedOut + ' exit=' + $attack.ExitCode)
        Add-Check 'protected_plaintext_archive_override.attack_no_ok_line' (($attack.Boot.OkCount -eq 0) -and ($attack.Boot.SinkOkCount -eq 0)) ('stdout_ok=' + $attack.Boot.OkCount + ' sink_ok=' + $attack.Boot.SinkOkCount)
        Add-Check 'protected_plaintext_archive_override.attack_failure_reason' (($attack.Boot.FailCount -eq 1) -and ($attack.Boot.FailReason -eq 'archive_unauthorized')) ('reason=' + $attack.Boot.FailReason + ' line=' + $attack.Boot.FailLine)
        Add-Check 'protected_plaintext_archive_override.attack_override_applied' ($attack.Boot.FailLine -match 'mode=unprotected') ('line=' + $attack.Boot.FailLine)
        Add-Check 'protected_plaintext_archive_override.attack_nonzero_exit' ($attack.ExitCode -ne 0) ('exit=' + $attack.ExitCode)
    }
}

# ---------------------------------------------------------------- bundling off

Invoke-Case 'bundling_off' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'bundling_off' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'bundling_off'
        Write-Preset -ProjectDir $proj -Name 'contract_disabled' -Runnable $true -Template $TemplatePath -EncryptPck $false -Bundle $false -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex ''
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_disabled' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'bundling_off' -Text $log
        Add-Check 'bundling_off.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
        $archives = @(Get-ArchiveList $out)
        Add-Check 'bundling_off.no_archive' ($archives.Count -eq 0) ('archives=' + $archives.Count)

        $stage = Copy-ReleaseStage -SourceDir $out -Name 'bundling_off'
        Assert-BootOk -Case 'bundling_off' -StageDir $stage -Mode 'unprotected' -MinEntries 0
        $boot = $script:LastBoot
        if ($null -ne $boot) {
            Add-Check 'bundling_off.entries_zero' ($boot.OkEntries -eq 0) ('entries=' + $boot.OkEntries)
            Add-Check 'bundling_off.no_key_entry' ($boot.ProbeKeySize -eq -1) ('toc_key=' + $boot.ProbeKeySize)
            Add-Check 'bundling_off.payloads_stay_in_pck' (($boot.ProbePayloadUsed -eq 'true') -and ($boot.ProbePayloadUnused -eq 'true')) ('used=' + $boot.ProbePayloadUsed + ' unused=' + $boot.ProbePayloadUnused)
        }
        $scan = Invoke-Scan -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'bundling_off.scan_ok' ($scan.Ok -and -not $scan.TimedOut) ('verdict=' + $scan.Verdict)
    }
}

# ---------------------------------------------------------------- protected, empty PCK key + stale archive

Invoke-Case 'protected_empty_key' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'protected_empty_key' $TemplateReason
    } elseif ($null -eq $script:ProtectedArchive) {
        Add-NotCovered 'protected_empty_key' 'no protected archive to plant as the stale copy'
    } else {
        $proj = New-ProjectCopy 'protected_empty_key'
        Write-Preset -ProjectDir $proj -Name 'contract_empty_key' -Runnable $true -Template $TemplatePath -EncryptInclude '.dspkg/*' -EncryptPck $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex ''
        $out = Join-Path $proj 'release'
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        $stalePath = Join-Path $out 'textures.dspkg'
        Copy-Item -LiteralPath $script:ProtectedArchive -Destination $stalePath -Force
        Add-Check 'stale_archive.planted_before_failed_export' (Test-Path -LiteralPath $stalePath) 'stale archive planted before the failed export'

        $run = Export-Release -ProjectDir $proj -Preset 'contract_empty_key' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'protected_empty_key' -Text $log
        Add-Check 'protected_empty_key.fail_closed_log' ($log -match 'Protection was requested but cannot be delivered') 'the exporter must refuse the preset'
        Add-Check 'stale_archive.deleted_by_failed_export' (-not (Test-Path -LiteralPath $stalePath)) 'a failed export must not leave a loadable archive'
        $archives = @(Get-ArchiveList $out)
        Add-Check 'protected_empty_key.no_archive' ($archives.Count -eq 0) ('archives=' + $archives.Count)

        $pck = Join-Path $out 'game.pck'
        $pckBytes = 0
        if (Test-Path -LiteralPath $pck) { $pckBytes = (Get-Item -LiteralPath $pck).Length }
        Add-Check 'protected_empty_key.pack_written' ($pckBytes -gt 0) ('pck_bytes=' + $pckBytes)
        if (($pckBytes -gt 0) -and ($pckBytes -lt 1024)) {
            $scan = Invoke-Scan -PackPath $pck
            Add-Check 'protected_empty_key.scan_rejects_truncated_pack' ((-not $scan.Ok) -and (($scan.Reason -eq 'bad_header') -or ($scan.Reason -eq 'zero_entries'))) ('verdict=' + $scan.Verdict)
        } else {
            # Measured here: 4.7.2 writes a complete, unencrypted PCK when
            # encrypt_pck=true has no key, rather than aborting after 112 bytes.
            # The truncated-pack expectation therefore does not reproduce; the
            # security property is still asserted - the pack carries no key.
            $scan = Invoke-Scan -PackPath $pck
            Add-Check 'protected_empty_key.scan_finds_no_plaintext_key' (($scan.Ok) -or ($scan.Reason -eq 'zero_entries')) ('verdict=' + $scan.Verdict)
            Add-KnownLimitation 'protected_empty_key.pack_truncation' ('engine wrote a complete PCK (' + $pckBytes + ' bytes, scan=' + $scan.Verdict + ') instead of the 112-byte truncation; the fail-closed payload removal and the missing_archive boot are covered')
        }
        $stage = Copy-ReleaseStage -SourceDir $out -Name 'protected_empty_key'
        Assert-BootFail -Case 'protected_empty_key' -StageDir $stage -Reason 'missing_archive' -Timeout 30
        $boot = $script:LastBoot
        if ($null -ne $boot) {
            Add-Check 'protected_empty_key.payloads_absent_from_pck' ($boot.ProbePayloadUsed -eq 'false') ('payload_used=' + $boot.ProbePayloadUsed)
        }
    }
}

# ---------------------------------------------------------------- include filter misses the key path

Invoke-Case 'include_filter_misses_key' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'include_filter_misses_key' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'include_filter_misses_key'
        Write-Preset -ProjectDir $proj -Name 'contract_include_miss' -Runnable $true -Template $TemplatePath -EncryptInclude '*.png' -EncryptPck $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_include_miss' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'include_filter_misses_key' -Text $log
        Add-Check 'include_filter_misses_key.fail_closed_log' ($log -match 'is not encrypted by the preset') 'the exporter must refuse the preset'
        $archives = @(Get-ArchiveList $out)
        Add-Check 'include_filter_misses_key.no_archive' ($archives.Count -eq 0) ('archives=' + $archives.Count)
        $pck = Join-Path $out 'game.pck'
        $probe = Invoke-PackProbe -PackPath $pck
        Add-Check 'include_filter_misses_key.bundle_declared' ($probe.Bundle -and $probe.Protected) ('bundle=' + $probe.Bundle + ' protected=' + $probe.Protected)
        Add-Check 'include_filter_misses_key.payload_absent_from_pck' (($probe.PayloadUsed -eq $false) -and ($probe.PayloadUnused -eq $false) -and ($probe.PayloadTrigger -eq $false)) ('used=' + $probe.PayloadUsed + ' unused=' + $probe.PayloadUnused + ' trigger=' + $probe.PayloadTrigger)
        $scan = Invoke-Scan -PackPath $pck
        Add-Check 'include_filter_misses_key.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
        Add-Check 'include_filter_misses_key.pack_verify_absent' (-not $verifyRegex.Match($log).Success) 'OUTCOME_FAILED must not emit PACK_VERIFY'
        $stage = Copy-ReleaseStage -SourceDir $out -Name 'include_filter_misses_key'
        Assert-BootFail -Case 'include_filter_misses_key' -StageDir $stage -Reason 'missing_archive'
    }
}

# ---------------------------------------------------------------- exclude filter removes the key path

Invoke-Case 'exclude_filter_removes_key' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'exclude_filter_removes_key' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'exclude_filter_removes_key'
        Write-Preset -ProjectDir $proj -Name 'contract_exclude_key' -Runnable $true -Template $TemplatePath -EncryptInclude '.dspkg/*' -EncryptExclude '.dspkg/toc.key' -EncryptPck $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_exclude_key' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'exclude_filter_removes_key' -Text $log
        Add-Check 'exclude_filter_removes_key.fail_closed_log' ($log -match 'is not encrypted by the preset') 'the exclude filter must be honoured'
        $archives = @(Get-ArchiveList $out)
        Add-Check 'exclude_filter_removes_key.no_archive' ($archives.Count -eq 0) ('archives=' + $archives.Count)
        $pck = Join-Path $out 'game.pck'
        $probe = Invoke-PackProbe -PackPath $pck
        Add-Check 'exclude_filter_removes_key.bundle_declared' ($probe.Bundle -and $probe.Protected) ('bundle=' + $probe.Bundle + ' protected=' + $probe.Protected)
        Add-Check 'exclude_filter_removes_key.payload_absent_from_pck' (($probe.PayloadUsed -eq $false) -and ($probe.PayloadUnused -eq $false) -and ($probe.PayloadTrigger -eq $false)) ('used=' + $probe.PayloadUsed + ' unused=' + $probe.PayloadUnused + ' trigger=' + $probe.PayloadTrigger)
        $scan = Invoke-Scan -PackPath $pck
        Add-Check 'exclude_filter_removes_key.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
        $stage = Copy-ReleaseStage -SourceDir $out -Name 'exclude_filter_removes_key'
        Assert-BootFail -Case 'exclude_filter_removes_key' -StageDir $stage -Reason 'missing_archive'
    }
}

# ---------------------------------------------------------------- partial salvage (mid-walk failure)

Invoke-Case 'partial_salvage' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'partial_salvage' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'partial_salvage'
        Write-Preset -ProjectDir $proj -Name 'contract_partial' -Runnable $true -Template $TemplatePath -EncryptPck $false -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex ''
        Write-Utf8NoBom -Path (Join-Path $proj 'assets\used.dstex') -Text 'BAD'
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_partial' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'partial_salvage' -Text $log
        Add-Check 'partial_salvage.bundling_aborted' ($log -match 'DSPkg bundling was aborted') 'a failed add must abort the walk'
        Add-Check 'partial_salvage.salvage_reported' ($log -match 'Deploying a PARTIAL archive') 'textures already skipped must not be lost'
        Add-Check 'partial_salvage.deployed' ($log -match 'Successfully deployed standalone textures archive') 'the partial archive must be deployed'
        $stage = Copy-ReleaseStage -SourceDir $out -Name 'partial_salvage'
        Assert-BootOk -Case 'partial_salvage' -StageDir $stage -Mode 'unprotected' -MinEntries 1
        $boot = $script:LastBoot
        if ($null -ne $boot) {
            Add-Check 'partial_salvage.entries_one' ($boot.OkEntries -eq 1) ('entries=' + $boot.OkEntries)
            Add-Check 'partial_salvage.failed_payload_stays_in_pck' ($boot.ProbePayloadUsed -eq 'true') ('payload_used=' + $boot.ProbePayloadUsed)
            Add-Check 'partial_salvage.salvaged_payload_leaves_pck' ($boot.ProbePayloadUnused -eq 'false') ('payload_unused=' + $boot.ProbePayloadUnused)
        }
        $probe = Invoke-PackProbe -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'partial_salvage.expects_archive' ($probe.Bundle -and (-not $probe.Protected)) ('bundle=' + $probe.Bundle)
        $scan = Invoke-Scan -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'partial_salvage.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
    }
}

# ---------------------------------------------------------------- subset exports

Invoke-Case 'subset_scenes' {
    $proj = New-ProjectCopy 'subset_scenes'
    Write-Preset -ProjectDir $proj -Name 'contract_scenes' -Filter 'scenes' -Files 'PackedStringArray("res://subset.tscn")' -EncryptPck $false -Bundle $true -Pairing $true
    Write-CredentialFile -ProjectDir $proj -KeyHex ''
    $out = Join-Path $proj 'release'
    $run = Export-Pack -ProjectDir $proj -Preset 'contract_scenes' -OutputDir $out
    Save-Log -Name 'subset_scenes' -Text (Get-ExportText $run)
    Add-Check 'subset_scenes.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
    $pck = Join-Path $out 'contract_scenes.pck'
    $archive = Join-Path $out 'textures.dspkg'
    Add-Check 'subset_scenes.archive_present' (Test-Path -LiteralPath $archive) 'archive must follow the scenes filter'
    $probe = Invoke-PackProbe -PackPath $pck
    Add-Check 'subset_scenes.bundle_declared' ($probe.Bundle -and (-not $probe.Protected)) ('bundle=' + $probe.Bundle)
    Add-Check 'subset_scenes.archive_id_nonzero' ($probe.ArchiveId -ne 0) ('archive_id=' + $probe.ArchiveId)
    $open = Invoke-ArchiveProbe -ProjectDir $proj -ArchivePath $archive -ExpectedId $probe.ArchiveId
    Add-Check 'subset_scenes.id_pairs_without_manifest' (($open.OpenErr -eq 0) -and ($open.Entries -eq 1)) ('open_err=' + $open.OpenErr + ' entries=' + $open.Entries + ' paths=' + ($open.EntryPaths -join ','))
    $scan = Invoke-Scan -PackPath $pck
    Add-Check 'subset_scenes.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
}

Invoke-Case 'subset_resources' {
    $proj = New-ProjectCopy 'subset_resources'
    Write-Preset -ProjectDir $proj -Name 'contract_resources' -Filter 'resources' -Files 'PackedStringArray("res://assets/used.dstex")' -EncryptPck $false -Bundle $true -Pairing $true
    Write-CredentialFile -ProjectDir $proj -KeyHex ''
    $out = Join-Path $proj 'release'
    $run = Export-Pack -ProjectDir $proj -Preset 'contract_resources' -OutputDir $out
    Save-Log -Name 'subset_resources' -Text (Get-ExportText $run)
    Add-Check 'subset_resources.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
    $pck = Join-Path $out 'contract_resources.pck'
    $archive = Join-Path $out 'textures.dspkg'
    Add-Check 'subset_resources.archive_present' (Test-Path -LiteralPath $archive) 'archive must follow the resources filter'
    $probe = Invoke-PackProbe -PackPath $pck
    Add-Check 'subset_resources.bundle_declared' ($probe.Bundle -and (-not $probe.Protected)) ('bundle=' + $probe.Bundle)
    Add-Check 'subset_resources.archive_id_nonzero' ($probe.ArchiveId -ne 0) ('archive_id=' + $probe.ArchiveId)
    $open = Invoke-ArchiveProbe -ProjectDir $proj -ArchivePath $archive -ExpectedId $probe.ArchiveId
    Add-Check 'subset_resources.id_pairs_without_manifest' (($open.OpenErr -eq 0) -and ($open.Entries -eq 1)) ('open_err=' + $open.OpenErr + ' entries=' + $open.Entries + ' paths=' + ($open.EntryPaths -join ','))
    $scan = Invoke-Scan -PackPath $pck
    Add-Check 'subset_resources.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
}

Invoke-Case 'subset_exclude' {
    $proj = New-ProjectCopy 'subset_exclude'
    Write-Preset -ProjectDir $proj -Name 'contract_exclude' -Filter 'exclude' -Files 'PackedStringArray("res://assets/unused.dstex", "res://assets/zzz_trigger.dstex")' -EncryptPck $false -Bundle $true -Pairing $true
    Write-CredentialFile -ProjectDir $proj -KeyHex ''
    $out = Join-Path $proj 'release'
    $run = Export-Pack -ProjectDir $proj -Preset 'contract_exclude' -OutputDir $out
    Save-Log -Name 'subset_exclude' -Text (Get-ExportText $run)
    Add-Check 'subset_exclude.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
    $pck = Join-Path $out 'contract_exclude.pck'
    $archive = Join-Path $out 'textures.dspkg'
    Add-Check 'subset_exclude.archive_present' (Test-Path -LiteralPath $archive) 'archive must follow the exclude filter'
    $probe = Invoke-PackProbe -PackPath $pck
    Add-Check 'subset_exclude.bundle_declared' ($probe.Bundle -and (-not $probe.Protected)) ('bundle=' + $probe.Bundle)
    Add-Check 'subset_exclude.excluded_payload_absent' ($probe.PayloadUnused -eq $false) ('payload_unused=' + $probe.PayloadUnused)
    $open = Invoke-ArchiveProbe -ProjectDir $proj -ArchivePath $archive -ExpectedId $probe.ArchiveId
    Add-Check 'subset_exclude.id_pairs_without_manifest' (($open.OpenErr -eq 0) -and ($open.Entries -eq 1)) ('open_err=' + $open.OpenErr + ' entries=' + $open.Entries + ' paths=' + ($open.EntryPaths -join ','))
    $scan = Invoke-Scan -PackPath $pck
    Add-Check 'subset_exclude.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
}

Invoke-Case 'subset_dedicated_server' {
    $proj = New-ProjectCopy 'subset_dedicated_server'
    Write-Preset -ProjectDir $proj -Name 'contract_server' -Filter 'customized' -Customized '{"res://assets/unused.dstex": "remove", "res://assets/zzz_trigger.dstex": "remove"}' -Dedicated $true -EncryptPck $false -Bundle $true -Pairing $true
    Write-CredentialFile -ProjectDir $proj -KeyHex ''
    $out = Join-Path $proj 'release'
    $run = Export-Pack -ProjectDir $proj -Preset 'contract_server' -OutputDir $out
    Save-Log -Name 'subset_dedicated_server' -Text (Get-ExportText $run)
    Add-Check 'subset_dedicated_server.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
    $pck = Join-Path $out 'contract_server.pck'
    $archive = Join-Path $out 'textures.dspkg'
    Add-Check 'subset_dedicated_server.archive_present' (Test-Path -LiteralPath $archive) 'archive must follow the customized filter'
    $probe = Invoke-PackProbe -PackPath $pck
    Add-Check 'subset_dedicated_server.bundle_declared' ($probe.Bundle -and (-not $probe.Protected)) ('bundle=' + $probe.Bundle)
    Add-Check 'subset_dedicated_server.removed_payload_absent' ($probe.PayloadUnused -eq $false) ('payload_unused=' + $probe.PayloadUnused)
    $open = Invoke-ArchiveProbe -ProjectDir $proj -ArchivePath $archive -ExpectedId $probe.ArchiveId
    Add-Check 'subset_dedicated_server.id_pairs_without_manifest' (($open.OpenErr -eq 0) -and ($open.Entries -eq 1)) ('open_err=' + $open.OpenErr + ' entries=' + $open.Entries + ' paths=' + ($open.EntryPaths -join ','))
    $scan = Invoke-Scan -PackPath $pck
    Add-Check 'subset_dedicated_server.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
}

# ---------------------------------------------------------------- skipped payload implies a declared archive

Invoke-Case 'skip_on_create_failure' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'skip_on_create_failure' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'skip_on_create_failure'
        Write-Preset -ProjectDir $proj -Name 'contract_create_fail' -EncryptInclude '.dspkg/*' -EncryptPck $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        # Hold the staging path open with no sharing. DSPkg.create() only
        # initializes builder state: it opens no file, so it still returns OK.
        $lockPath = Join-Path $out 'textures.dspkg.tmp'
        Set-Content -LiteralPath $lockPath -Value '' -NoNewline -Encoding Ascii
        $lock = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        try {
            $run = Export-Pack -ProjectDir $proj -Preset 'contract_create_fail' -OutputDir $out
        } finally {
            $lock.Dispose()
        }
        $log = Get-ExportText $run
        Save-Log -Name 'skip_on_create_failure' -Text $log
        Add-Check 'skip_on_create_failure.create_is_not_io' (($log -match 'Initialized DSPkg staging archive') -and -not ($log -match 'Failed to create DSPkg staging archive')) 'create() returned OK with the staging path held under an exclusive lock'
        Add-KnownLimitation 'skip_on_create_failure' 'DSPkg.create() initializes builder state only and opens no file, so its failure branch cannot be reached from outside the builder; the exporter then failed at the rename (measured: staging path locked, finalize wrote a unique temp, rename failed). The contract consequence is still asserted below.'
        $pck = Join-Path $out 'contract_create_fail.pck'
        $probe = Invoke-PackProbe -PackPath $pck
        Add-Check 'skip_on_create_failure.expects_archive' ($probe.Bundle -and $probe.Protected) ('bundle=' + $probe.Bundle + ' protected=' + $probe.Protected)
        Add-Check 'skip_on_create_failure.payloads_skipped' (($probe.PayloadUsed -eq $false) -and ($probe.PayloadUnused -eq $false)) ('used=' + $probe.PayloadUsed + ' unused=' + $probe.PayloadUnused)
        Add-Check 'skip_on_create_failure.no_deployed_archive' (@(Get-ArchiveList $out).Count -eq 0) 'the failed export must not deploy an archive'
        $scan = Invoke-Scan -PackPath $pck
        Add-Check 'skip_on_create_failure.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
    }
}

Invoke-Case 'skip_on_first_add_failure' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'skip_on_first_add_failure' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'skip_on_first_add_failure'
        Write-Preset -ProjectDir $proj -Name 'contract_first_add_fail' -EncryptInclude '.dspkg/*' -EncryptPck $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        foreach ($asset in @('used.dstex', 'unused.dstex', 'zzz_trigger.dstex')) {
            Write-Utf8NoBom -Path (Join-Path $proj ('assets\' + $asset)) -Text 'BAD'
        }
        $out = Join-Path $proj 'release'
        $run = Export-Pack -ProjectDir $proj -Preset 'contract_first_add_fail' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'skip_on_first_add_failure' -Text $log
        Add-Check 'skip_on_first_add_failure.first_add_failed' ($log -match 'Failed to add texture to archive') 'injected first add_texture() failure'
        Add-Check 'skip_on_first_add_failure.no_archive' (@(Get-ArchiveList $out).Count -eq 0) 'no archive may be deployed'
        $pck = Join-Path $out 'contract_first_add_fail.pck'
        $probe = Invoke-PackProbe -PackPath $pck
        Add-Check 'skip_on_first_add_failure.expects_archive' ($probe.Bundle -and $probe.Protected) ('bundle=' + $probe.Bundle + ' protected=' + $probe.Protected)
        Add-Check 'skip_on_first_add_failure.payloads_skipped' (($probe.PayloadUsed -eq $false) -and ($probe.PayloadUnused -eq $false)) ('used=' + $probe.PayloadUsed + ' unused=' + $probe.PayloadUnused)
        $scan = Invoke-Scan -PackPath $pck
        Add-Check 'skip_on_first_add_failure.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
    }
}

Invoke-Case 'skip_on_finalize_failure' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'skip_on_finalize_failure' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'skip_on_finalize_failure'
        Write-Preset -ProjectDir $proj -Name 'contract_finalize_fail' -EncryptInclude '.dspkg/*' -EncryptPck $true -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex $TemplateKey
        $out = Join-Path $proj 'release'
        $run = Export-Pack -ProjectDir $proj -Preset 'contract_finalize_fail' -OutputDir $out -Environment @{
            DSPKG_CONTRACT_MUTATE_TARGET = (Join-Path $proj 'assets\used.dstex')
        }
        $log = Get-ExportText $run
        Save-Log -Name 'skip_on_finalize_failure' -Text $log
        Add-Check 'skip_on_finalize_failure.mutation_injected' ($log -match '\[contract-injector\] mutated=') 'the staged payload must be mutated after add and before finalize'
        Add-Check 'skip_on_finalize_failure.finalize_failed' ($log -match 'failed to finalize DSPkg staging archive') 'finalize() must fail on the mutated source'
        Add-Check 'skip_on_finalize_failure.mutation_guard_reason' ($log -match 'Source texture changed after it was added') 'the builder mutation guard must report the cause'
        Add-Check 'skip_on_finalize_failure.no_archive' (@(Get-ArchiveList $out).Count -eq 0) 'a failed finalize must not deploy an archive'
        $pck = Join-Path $out 'contract_finalize_fail.pck'
        $probe = Invoke-PackProbe -PackPath $pck
        Add-Check 'skip_on_finalize_failure.expects_archive' ($probe.Bundle -and $probe.Protected) ('bundle=' + $probe.Bundle + ' protected=' + $probe.Protected)
        Add-Check 'skip_on_finalize_failure.payloads_skipped' (($probe.PayloadUsed -eq $false) -and ($probe.PayloadUnused -eq $false)) ('used=' + $probe.PayloadUsed + ' unused=' + $probe.PayloadUnused)
        $scan = Invoke-Scan -PackPath $pck
        Add-Check 'skip_on_finalize_failure.scan_ok' ($scan.Ok) ('verdict=' + $scan.Verdict)
    }
}

# ---------------------------------------------------------------- engine error before _export_end
#
# _core_export_reports_error() must stop the archive from being deployed when the
# engine recorded an EXPORT_MESSAGE_ERROR for this run. The gate cannot make a
# real PCK write fail after the texture walk, so the sibling fixture plugin (which
# is registered first, so its _export_end runs first) records an error through the
# same platform message list the engine uses for its own template/PCK failures.
# This exercises the message-count snapshot, the worst-type comparison and the
# fail-closed branch together.

Invoke-Case 'engine_error_fail_closed' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'engine_error_fail_closed' $TemplateReason
    } else {
        $proj = New-ProjectCopy 'engine_error_fail_closed'
        Write-Preset -ProjectDir $proj -Name 'contract_engine_error' -Runnable $true -Template $TemplatePath -EncryptPck $false -Bundle $true -Pairing $true
        Write-CredentialFile -ProjectDir $proj -KeyHex ''
        $out = Join-Path $proj 'release'
        $run = Export-Release -ProjectDir $proj -Preset 'contract_engine_error' -OutputDir $out -Environment @{
            DSPKG_CONTRACT_ENGINE_ERROR = 'injected engine export failure'
        }
        $log = Get-ExportText $run
        Save-Log -Name 'engine_error_fail_closed' -Text $log
        Add-Check 'engine_error_fail_closed.error_injected' ($log -match '\[contract-injector\] engine_error=injected engine export failure') 'the sibling plugin must record the error before the add-on sees it'
        Add-Check 'engine_error_fail_closed.engine_error_recorded' ($log -match 'ERROR: Contract Injector: injected engine export failure') 'the platform must carry the injected EXPORT_MESSAGE_ERROR'
        Add-Check 'engine_error_fail_closed.fail_closed_log' ($log -match 'the export failed before .* could be deployed') 'the add-on must refuse to deploy after an engine error'
        Add-Check 'engine_error_fail_closed.no_archive' (@(Get-ArchiveList $out).Count -eq 0) ('archives=' + @(Get-ArchiveList $out).Count)
        Add-Check 'engine_error_fail_closed.no_staging' (@(Get-ChildItem -LiteralPath $out -Filter '*.dspkg.tmp' -File -ErrorAction SilentlyContinue).Count -eq 0) 'the staging file must be removed'
        Add-Check 'engine_error_fail_closed.failure_escalated' ($log -match 'DirectStorage archive step failed') 'the export must be reported as failed to the platform'
        Add-Check 'engine_error_fail_closed.engine_steps_completed' ((Test-Path -LiteralPath (Join-Path $out 'game.exe')) -and (Test-Path -LiteralPath (Join-Path $out 'game.pck'))) 'the engine exe/PCK steps still complete; only the archive is withheld'
    }
}

# ---------------------------------------------------------------- corrupted TOC

Invoke-Case 'corrupted_toc' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'corrupted_toc' $TemplateReason
    } elseif ($null -eq $script:ProtectedStage) {
        Add-NotCovered 'corrupted_toc' 'no protected release to corrupt'
    } else {
        $stage = Join-Path $WorkRoot 'stage-corrupted_toc'
        if (Test-Path -LiteralPath $stage) { throw ('stage directory already exists: ' + $stage) }
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        foreach ($item in @(Get-ChildItem -LiteralPath $script:ProtectedStage -File)) {
            Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $stage $item.Name) -Force
        }
        $archivePath = Join-Path $stage 'textures.dspkg'
        $bytes = [System.IO.File]::ReadAllBytes($archivePath)
        # v4 header: magic u32, version u16, flags u16, entry_count u32 (8..11),
        # data_offset u64 (12..19). The TOC is [64, data_offset).
        $dataOffset = [BitConverter]::ToUInt64($bytes, 12)
        $target = 72
        if ($dataOffset -le ($target + 1)) { throw ('archive TOC is too small to corrupt: data_offset=' + $dataOffset) }
        $bytes[$target] = $bytes[$target] -bxor 0xFF
        [System.IO.File]::WriteAllBytes($archivePath, $bytes)
        Add-Check 'corrupted_toc.toc_byte_flipped' $true ('offset=' + $target + ' data_offset=' + $dataOffset + ' bytes=' + $bytes.Length)
        Add-Check 'corrupted_toc.toc_range_sane' (($dataOffset -gt 64) -and ($dataOffset -le $bytes.Length)) ('data_offset=' + $dataOffset + ' bytes=' + $bytes.Length)
        Assert-BootFail -Case 'corrupted_toc' -StageDir $stage -Reason 'archive_unauthorized'
        $scan = Invoke-Scan -PackPath (Join-Path $stage 'game.pck')
        Add-Check 'corrupted_toc.pack_still_valid' ($scan.Ok) ('verdict=' + $scan.Verdict)
    }
}

# ---------------------------------------------------------------- alert bypass

Invoke-Case 'alert_bypass' {
    if (-not $ProtectedAvailable) {
        Add-NotCovered 'alert_bypass' $TemplateReason
    } elseif ($null -eq $script:ProtectedStage) {
        Add-NotCovered 'alert_bypass' 'no protected release to stage without its archive'
    } else {
        $stage = Join-Path $WorkRoot 'stage-alert_bypass'
        if (Test-Path -LiteralPath $stage) { throw ('stage directory already exists: ' + $stage) }
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        foreach ($item in @(Get-ChildItem -LiteralPath $script:ProtectedStage -File)) {
            Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $stage $item.Name) -Force
        }
        Remove-Item -LiteralPath (Join-Path $stage 'textures.dspkg') -Force
        if ($NegativeControl) {
            # Self-test: with the archive left in place the boot succeeds, so the
            # case's missing_archive expectation must fail the gate. A green run
            # here would mean the assertion is vacuous.
            Copy-Item -LiteralPath $script:ProtectedArchive -Destination (Join-Path $stage 'textures.dspkg') -Force
            Write-Output '[gate] negative control: the archive was left in place on purpose'
        }
        Add-Check 'alert_bypass.archive_absent' (-not (Test-Path -LiteralPath (Join-Path $stage 'textures.dspkg'))) 'premise: the declared archive is missing'
        $result = Invoke-Boot -StageDir $stage -UserArgs '--dspkg-validate=protected' -Timeout 25
        $boot = $result.Boot
        Add-Check 'alert_bypass.exits_without_dialog' ((-not $result.TimedOut) -and (-not $boot.TimedOut)) ('timed_out=' + $boot.TimedOut + ' exit=' + $result.ExitCode)
        Add-Check 'alert_bypass.failure_reason' (($boot.FailCount -eq 1) -and ($boot.FailReason -eq 'missing_archive')) ('line=' + $boot.FailLine)
        Add-Check 'alert_bypass.no_ok_line' ($boot.OkCount -eq 0) ('stdout_ok=' + $boot.OkCount)
        Add-Check 'alert_bypass.no_dialog_marker' (-not ($boot.Combined -match 'ALERT_BEGIN|ALERT_END')) 'no alert marker may appear in a headless run'
        # The exit code is supporting evidence only: a later quit(0) from the
        # game's own scene overrides it (measured: this run exits 0 while the
        # failure line is correct). The sentinel is the pass criterion.
    }
}

# ---------------------------------------------------------------- magic parity

Invoke-Case 'magic_parity' {
    $arguments = '--headless --path "' + $MasterProject + '" --script "' + (Join-Path $CaseScripts 'magic_parity.gd') + '"'
    $run = Invoke-Tool -Exe $Godot -Arguments $arguments -WorkingDirectory $MasterProject -TimeoutSec 120 -Environment @{ DSPKG_CONTRACT_WORKSPACE = $WorkspaceRoot }
    $text = Get-ExportText $run
    Save-Log -Name 'magic_parity' -Text $text
    $lineRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^MAGIC_PARITY labels_match=(\w+) sizes_match=(\w+) digests_match=(\w+) match=(\w+)\s*$')
    $labelRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^MAGIC_PARITY label=(\S+)')
    $line = $lineRegex.Match($text)
    $detail = ($text -split "`n" | Where-Object { $_ -match '^MAGIC_PARITY' }) -join ' | '
    Add-Check 'magic_parity.parsed' $line.Success $detail
    Add-Check 'magic_parity.cpp_gdscript_scanner_agree' ($line.Success -and ($line.Groups[4].Value -eq 'true')) 'the three derivations must produce identical 16-byte magic'
    $labels = $labelRegex.Match($text)
    Add-Check 'magic_parity.label_sources_found' ($labels.Success -and -not [string]::IsNullOrEmpty($labels.Groups[1].Value)) ('label=' + $labels.Groups[1].Value)
}

# ---------------------------------------------------------------- key-scan positive control

Invoke-Case 'key_scan_control' {
    if (-not (Test-Path -LiteralPath (Join-Path $ControlFixture 'addons\control_exporter\plugin.cfg'))) {
        Add-NotCovered 'key_scan_control' 'control fixture missing: project/headless_tests/export_contract/control_fixture/key_scan_control'
    } else {
        $proj = Join-Path $WorkRoot 'case-key_scan_control'
        if (Test-Path -LiteralPath $proj) { throw ('case directory already exists: ' + $proj) }
        Copy-Item -LiteralPath $ControlFixture -Destination $proj -Recurse -Force
        Write-Preset -ProjectDir $proj -Name 'control_leak' -EncryptInclude '*.png' -EncryptPck $true -Bundle $false -Pairing $false
        Write-CredentialFile -ProjectDir $proj -KeyHex $ControlKey
        $out = Join-Path $proj 'release'
        $run = Export-Pack -ProjectDir $proj -Preset 'control_leak' -OutputDir $out
        $log = Get-ExportText $run
        Save-Log -Name 'key_scan_control' -Text $log
        Add-Check 'key_scan_control.export_exit' (($run.ExitCode -eq 0) -and -not $run.TimedOut) ('exit=' + $run.ExitCode)
        Add-Check 'key_scan_control.key_injected' ($log -match '\[key-scan-control\] injected=res://\.dspkg/toc\.key len=48') 'the fixture must write the 48-byte key entry'
        $pck = Join-Path $out 'control_leak.pck'
        $pckBytes = 0
        if (Test-Path -LiteralPath $pck) { $pckBytes = (Get-Item -LiteralPath $pck).Length }
        Add-Check 'key_scan_control.pack_written' ($pckBytes -gt 1024) ('pck_bytes=' + $pckBytes)
        $scan = Invoke-Scan -PackPath $pck
        Add-Check 'key_scan_control.scan_detects_plaintext_key' ((-not $scan.Ok) -and ($scan.Reason -eq 'key_magic_found')) ('verdict=' + $scan.Verdict)
    }
}

# ---------------------------------------------------------------- footer

$failed = @($script:Results | Where-Object { -not $_.Pass })
$coversList = @($script:Covered | Sort-Object -Unique)
$notCoveredText = 'none'
if ($script:NotCovered.Count -gt 0) { $notCoveredText = ($script:NotCovered -join '; ') }
$knownLimitsText = 'none'
if ($script:KnownLimits.Count -gt 0) { $knownLimitsText = ($script:KnownLimits -join '; ') }

Write-Output ''
Write-Output ('[gate] checks=' + $script:Results.Count + ' failed=' + $failed.Count)
foreach ($failure in $failed) {
    Write-Output ('[gate] FAILED: ' + $failure.Name + ' :: ' + (($failure.Detail -replace "`r?`n", ' | ')))
}
Write-Output ''
Write-Output ('covers=' + ($coversList -join ','))
Write-Output ('not_covered=' + $notCoveredText)
Write-Output ('known_limits=' + $knownLimitsText)
Write-Output ''
if ($failed.Count -gt 0) {
    Write-Output ('[gate] RESULT FAIL checks=' + $script:Results.Count + ' failed=' + $failed.Count)
    exit 1
}
if ($RequireFullCoverage -and $script:NotCovered.Count -gt 0) {
    Write-Output ('[gate] RESULT FAIL coverage_required checks=' + $script:Results.Count + ' not_covered=' + $script:NotCovered.Count)
    exit 1
}
Write-Output ('[gate] RESULT PASS checks=' + $script:Results.Count)
if ($Cleanup) {
    Write-Output ('[gate] cleanup: removing ' + $WorkRoot)
    try {
        Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction Stop
    } catch {
        Write-Output ('[gate] cleanup failed (artifacts kept): ' + $_.Exception.Message)
    }
}
exit 0
