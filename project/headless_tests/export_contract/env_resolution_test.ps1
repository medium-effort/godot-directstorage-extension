#requires -Version 5.1
[CmdletBinding()]
param(
    # A real 4.7.2 keyed release template. Runs 1, 3, 4, 5 and 7 reach an export, so this
    # is a workstation test, not a fresh-clone test. Runs 2 and 6 stay runnable without one.
    [string]$KeyedTemplate = '',
    [string]$KeyFile = '',
    # Root for the gate's work roots and the .env fixtures. Default: out/env-<timestamp>.
    [string]$OutRoot = ''
)

# Closure test for the export-contract gate's keyed-template input resolution:
#   parameter > process environment > workspace .env > built-in default
#   * the environment variables are actually consulted (and the banner says so)
#   * a bad environment or .env value stops the run before any case
#   * an argument still wins, and its missing-path semantics are unchanged
#   * an empty .env value counts as unset
#   * the existing 64-hex validation stays the only hex rule
#
# The gate runs as a child process: `& <gate>` would run its `exit` in this session.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$WorkspaceRoot = $Here
while (-not (Test-Path -LiteralPath (Join-Path $WorkspaceRoot 'SConstruct'))) {
    $Parent = Split-Path -Parent $WorkspaceRoot
    if ([string]::IsNullOrEmpty($Parent) -or $Parent -eq $WorkspaceRoot) { throw 'Workspace root not found.' }
    $WorkspaceRoot = $Parent
}

$Gate = Join-Path $Here 'run_export_contract.ps1'
if (-not (Test-Path -LiteralPath $Gate)) { throw ('Gate script missing: ' + $Gate) }

if ([string]::IsNullOrEmpty($KeyedTemplate)) {
    $KeyedTemplate = Join-Path $WorkspaceRoot '.agents\scratch\test_keyed_template_release\godot.windows.template_release.x86_64.exe'
}
if ([string]::IsNullOrEmpty($KeyFile)) {
    $KeyFile = Join-Path (Split-Path -Parent $KeyedTemplate) 'key.txt'
}
if ([string]::IsNullOrEmpty($OutRoot)) {
    $OutRoot = Join-Path $Here ('out\env-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null
$OutRoot = (Resolve-Path -LiteralPath $OutRoot).Path

if (-not (Test-Path -LiteralPath $KeyedTemplate)) {
    Write-Output ('[test] SKIP prerequisite: keyed release template not found: ' + $KeyedTemplate)
    Write-Output '[test] Runs 1, 3, 4, 5 and 7 need a real keyed template; this harness cannot prove them without one.'
    exit 2
}

$RealTemplate = (Resolve-Path -LiteralPath $KeyedTemplate).Path
$MissingTemplate = Join-Path $OutRoot 'no-template\godot.windows.template_release.x86_64.exe'
$MissingKeyFile = Join-Path $OutRoot 'no-key\key.txt'

# Fixtures: a .env that names the real template, one with an empty value, and a key file
# with 63 hex characters. UTF-8 without BOM, CRLF-free (the reader trims either way).
$EnvReal = Join-Path $OutRoot 'env-real.env'
$EnvEmpty = Join-Path $OutRoot 'env-empty.env'
$ShortKey = Join-Path $OutRoot 'short-key\key.txt'
$Encoder = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($EnvReal, ('DSPKG_KEYED_TEMPLATE=' + $RealTemplate + "`n"), $Encoder)
[System.IO.File]::WriteAllText($EnvEmpty, ("DSPKG_KEYED_TEMPLATE=`nDSPKG_KEYED_TEMPLATE_KEY_FILE=`n"), $Encoder)
New-Item -ItemType Directory -Path (Split-Path -Parent $ShortKey) -Force | Out-Null
[System.IO.File]::WriteAllText($ShortKey, ('0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde' + "`n"), $Encoder)

$script:Checks = New-Object System.Collections.Generic.List[object]

function Add-TestCheck {
    param([string]$Name, [bool]$Pass, [string]$Detail = '')
    $script:Checks.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
    $tag = if ($Pass) { 'PASS' } else { 'FAIL' }
    if ([string]::IsNullOrEmpty($Detail)) {
        Write-Output ('[test] ' + $tag + ' ' + $Name)
    } else {
        Write-Output ('[test] ' + $tag + ' ' + $Name + ' :: ' + $Detail)
    }
}

function Invoke-Gate {
    param([string[]]$GateArguments, [hashtable]$Environment = @{})
    $names = @('DSPKG_KEYED_TEMPLATE', 'DSPKG_KEYED_TEMPLATE_KEY_FILE')
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
    $raw = @()
    $code = -1
    try {
        foreach ($name in $names) {
            $value = $null
            if ($Environment.ContainsKey($name)) { $value = [string]$Environment[$name] }
            [Environment]::SetEnvironmentVariable($name, $value)
        }
        # Native stderr (Godot notices) must not abort the harness under
        # $ErrorActionPreference = 'Stop', so relax it for the child.
        $previousEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $raw = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Gate @GateArguments 2>&1
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousEap
        }
    } finally {
        foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    }
    if ($null -eq $code) { $code = -1 }
    return [pscustomobject]@{ Output = (($raw | Out-String)); ExitCode = [int]$code }
}

# Normalised for banner comparison: the gate prints resolved paths with forward slashes.
function Get-NormalisedPath {
    param([string]$Path)
    if ([string]::IsNullOrEmpty($Path)) { return '' }
    return ($Path.Replace('\', '/')).TrimEnd('/')
}

function Get-BannerSource {
    param([string]$Text)
    $match = [regex]::Match($Text, '(?m)^\[gate\] keyed_template=(.+?) \(source=([\w.]+)\)\r?$')
    if ($match.Success) {
        return [pscustomobject]@{ Path = $match.Groups[1].Value.Trim(); Source = $match.Groups[2].Value }
    }
    return [pscustomobject]@{ Path = ''; Source = '' }
}

# ---- 1: the environment variable is consulted and announced.
Write-Output ''
Write-Output '[test] run 1: $env:DSPKG_KEYED_TEMPLATE=<real>, no -KeyedTemplate'
$run1 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run1')) -Environment @{ DSPKG_KEYED_TEMPLATE = $RealTemplate }
$banner1 = Get-BannerSource $run1.Output
Add-TestCheck 'run1.exit_zero' ($run1.ExitCode -eq 0) ('exit=' + $run1.ExitCode)
Add-TestCheck 'run1.banner_names_environment_path' ((Get-NormalisedPath $banner1.Path) -eq (Get-NormalisedPath $RealTemplate)) ('banner=' + $banner1.Path + ' expected=' + $RealTemplate)
Add-TestCheck 'run1.banner_source_environment' ($banner1.Source -eq 'environment') ('source=' + $banner1.Source)
Add-TestCheck 'run1.result_pass' ($run1.Output -match '(?m)^\[gate\] RESULT PASS ') 'RESULT PASS must be printed'
Add-TestCheck 'run1.no_not_covered' ($run1.Output -match '(?m)^not_covered=none\r?$') 'the protected case must be covered'
Add-TestCheck 'run1.checks_recorded' ($run1.Output -match '(?m)^\[gate\] checks=[1-9]\d* failed=0\r?$') 'a covered protected case records checks'

# ---- 2: a bad environment value is fatal, before any case runs.
Write-Output ''
Write-Output '[test] run 2: $env:DSPKG_KEYED_TEMPLATE=<missing>'
$run2 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run2')) -Environment @{ DSPKG_KEYED_TEMPLATE = $MissingTemplate }
Add-TestCheck 'run2.exit_nonzero' ($run2.ExitCode -eq 1) ('exit=' + $run2.ExitCode)
Add-TestCheck 'run2.fail_input_line' ($run2.Output -match '(?m)^\[gate\] RESULT FAIL input DSPKG_KEYED_TEMPLATE=') 'the failing input must be named'
Add-TestCheck 'run2.no_result_pass' (-not ($run2.Output -match '(?m)^\[gate\] RESULT PASS ')) 'a bad input must not report PASS'

# ---- 3: the key-file variable is validated separately.
Write-Output ''
Write-Output '[test] run 3: $env:DSPKG_KEYED_TEMPLATE_KEY_FILE=<missing> with a real template'
$run3 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run3')) -Environment @{ DSPKG_KEYED_TEMPLATE = $RealTemplate; DSPKG_KEYED_TEMPLATE_KEY_FILE = $MissingKeyFile }
Add-TestCheck 'run3.exit_nonzero' ($run3.ExitCode -eq 1) ('exit=' + $run3.ExitCode)
Add-TestCheck 'run3.fail_input_line' ($run3.Output -match '(?m)^\[gate\] RESULT FAIL input DSPKG_KEYED_TEMPLATE_KEY_FILE=') 'the failing key input must be named'
Add-TestCheck 'run3.no_result_pass' (-not ($run3.Output -match '(?m)^\[gate\] RESULT PASS ')) 'a bad key path must not report PASS'

# ---- 4: an argument outranks the environment and keeps its degrade semantics.
Write-Output ''
Write-Output '[test] run 4: -KeyedTemplate <missing> with $env:DSPKG_KEYED_TEMPLATE=<real>'
$run4 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run4'), '-KeyedTemplate', $MissingTemplate) -Environment @{ DSPKG_KEYED_TEMPLATE = $RealTemplate }
Add-TestCheck 'run4.exit_zero' ($run4.ExitCode -eq 0) ('exit=' + $run4.ExitCode)
Add-TestCheck 'run4.parameter_wins' ($run4.Output -match ('(?m)^\[gate\] keyed_template=unavailable \(keyed release template missing: ' + [regex]::Escape($MissingTemplate) + '\)')) 'the banner must name the argument, not the environment value'
Add-TestCheck 'run4.parameter_still_degrades' ($run4.Output -match '(?m)^not_covered=protected_success \(keyed release template missing: ') 'an explicitly missing parameter must still become a coverage gap'

# ---- 5: the workspace .env participates.
Write-Output ''
Write-Output '[test] run 5: -EnvFilePath <fixture> naming the real template, process environment cleared'
$run5 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run5'), '-EnvFilePath', $EnvReal) -Environment @{}
$banner5 = Get-BannerSource $run5.Output
Add-TestCheck 'run5.exit_zero' ($run5.ExitCode -eq 0) ('exit=' + $run5.ExitCode)
Add-TestCheck 'run5.banner_names_env_file_path' ((Get-NormalisedPath $banner5.Path) -eq (Get-NormalisedPath $RealTemplate)) ('banner=' + $banner5.Path)
Add-TestCheck 'run5.banner_source_env_file' ($banner5.Source -eq '.env') ('source=' + $banner5.Source)
Add-TestCheck 'run5.result_pass' ($run5.Output -match '(?m)^\[gate\] RESULT PASS ') 'RESULT PASS must be printed'

# ---- 6: an empty .env value counts as unset.
Write-Output ''
Write-Output '[test] run 6: -EnvFilePath <fixture with DSPKG_KEYED_TEMPLATE=> and no process environment'
$run6 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run6'), '-EnvFilePath', $EnvEmpty) -Environment @{}
Add-TestCheck 'run6.no_input_failure' (-not ($run6.Output -match '(?m)^\[gate\] RESULT FAIL input ')) 'an empty value is unset, not a bad input'
Add-TestCheck 'run6.banner_names_built_in_default' ($run6.Output -match '(?m)^\[gate\] keyed_template=unavailable \(keyed release template missing: .*test_keyed_template_release') 'the built-in default must be used'
Add-TestCheck 'run6.exit_zero' ($run6.ExitCode -eq 0) ('exit=' + $run6.ExitCode)

# ---- 7: the existing validator still owns the hex rule.
Write-Output ''
Write-Output '[test] run 7: real template with a 63-hex key file'
$run7 = Invoke-Gate -GateArguments @('-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'run7'), '-KeyedTemplate', $RealTemplate, '-KeyFile', $ShortKey) -Environment @{}
Add-TestCheck 'run7.exit_zero' ($run7.ExitCode -eq 0) ('exit=' + $run7.ExitCode)
Add-TestCheck 'run7.hex_rule_reported' ($run7.Output -match '(?m)^not_covered=protected_success \(template key is not 64 hex characters\)') 'the validator, not the resolver, must own the hex rule'

$failed = @($script:Checks | Where-Object { -not $_.Pass })
Write-Output ''
Write-Output ('[test] assertions=' + $script:Checks.Count + ' failed=' + $failed.Count)
if ($failed.Count -gt 0) {
    foreach ($failure in $failed) {
        Write-Output ('[test] FAILED: ' + $failure.Name + ' :: ' + $failure.Detail)
    }
    Write-Output '[test] RESULT FAIL'
    exit 1
}
Write-Output '[test] RESULT PASS'
exit 0
