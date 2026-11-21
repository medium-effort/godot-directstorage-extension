#requires -Version 5.1
[CmdletBinding()]
param(
    # Root for the gate's work roots. Default: out/self-test-<timestamp> (gitignored).
    [string]$OutRoot = '',
    # Keep the work roots and the captured gate output after a passing run.
    [switch]$KeepRoots
)

# Closure test for the export-contract gate's verdict ladder:
#   * a run whose protected case was skipped fails instead of reporting PASS
#   * RESULT PARTIAL appears only under the explicit -AllowPartial opt-out
#   * a run that executed no check fails, opt-out or not
#   * a subset run that really executed a case is not reported as PASS
#   * the healthy subset still records its checks
#
# No keyed template is required: the failing legs pass a deliberately missing path and the
# healthy legs use key_scan_control, which needs only the Godot console binary and the
# built extension DLLs.

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
if ([string]::IsNullOrEmpty($OutRoot)) {
    $OutRoot = Join-Path $Here ('out\self-test-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null
$OutRoot = (Resolve-Path -LiteralPath $OutRoot).Path
$MissingTemplate = Join-Path $OutRoot 'no-template\godot.exe'

$script:Asserts = New-Object System.Collections.Generic.List[object]

function Add-Assert {
    param([string]$Name, [bool]$Pass, [string]$Detail = '')
    $script:Asserts.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
    $tag = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Output ('[self-test] ' + $tag + ' ' + $Name + ' :: ' + $Detail)
}

# The gate runs as a child process: `& <gate>` would execute its `exit` in this session.
function Invoke-GateRun {
    param([string]$Name, [string[]]$Arguments)
    # Windows PowerShell 5.1 turns a native command's stderr into a NativeCommandError, which
    # $ErrorActionPreference = 'Stop' promotes to a terminating error (a deliberately rejected
    # argument is enough to trigger it). Relax it for the child only; the exit code and the
    # captured text are the verdict.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $captured = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Gate @Arguments 2>&1
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    $text = ($captured | ForEach-Object { [string]$_ }) -join "`n"
    $logPath = Join-Path $OutRoot ($Name + '.log')
    [System.IO.File]::WriteAllText($logPath, $text, (New-Object System.Text.UTF8Encoding($false)))
    return [pscustomobject]@{ Name = $Name; ExitCode = $LASTEXITCODE; Output = $text }
}

# ---- 1-5: a skipped protected case fails closed, and never reads PASS.
Write-Output ''
Write-Output '[test] run 1: -Only protected_success with a missing template (default verdict)'
$r1 = Invoke-GateRun 'missing_template_default' @('-KeyedTemplate', $MissingTemplate, '-Only', 'protected_success', '-WorkRoot', (Join-Path $OutRoot 'r1'))
Add-Assert 'missing_template_default.exit_one' ($r1.ExitCode -eq 1) ('exit=' + $r1.ExitCode)
Add-Assert 'missing_template_default.fails_closed' ($r1.Output -match '(?m)^\[gate\] RESULT FAIL coverage_incomplete') 'the run must fail on coverage'
Add-Assert 'missing_template_default.gap_named' ($r1.Output -match '(?m)^not_covered=protected_success \(keyed release template missing') 'the gap must name the case and reason'
Add-Assert 'missing_template_default.no_pass' (-not ($r1.Output -match '(?m)^\[gate\] RESULT PASS ')) 'a skipped case must not print RESULT PASS'
Add-Assert 'missing_template_default.zero_checks' ($r1.Output -match '(?m)^\[gate\] cases=\d+/\d+ checks=0 failed=0') 'the case ran no check'

# ---- 6-8: the opt-out reports PARTIAL and still never reads PASS.
Write-Output ''
Write-Output '[test] run 2: the same run with -AllowPartial'
$r2 = Invoke-GateRun 'missing_template_partial_allowed' @('-KeyedTemplate', $MissingTemplate, '-Only', 'protected_success', '-AllowPartial', '-WorkRoot', (Join-Path $OutRoot 'r2'))
Add-Assert 'missing_template_partial_allowed.exit_zero' ($r2.ExitCode -eq 0) ('exit=' + $r2.ExitCode)
Add-Assert 'missing_template_partial_allowed.partial' ($r2.Output -match '(?m)^\[gate\] RESULT PARTIAL ') 'the opt-out must be labelled PARTIAL'
Add-Assert 'missing_template_partial_allowed.no_pass' (-not ($r2.Output -match '(?m)^\[gate\] RESULT PASS ')) 'PARTIAL must never be printed as PASS'

# ---- 9-11: a filter that matched nothing always fails.
Write-Output ''
Write-Output '[test] run 3: -Only with a name no case has (default verdict)'
$r3 = Invoke-GateRun 'subset_typo_default' @('-Only', '__no_such_case__', '-WorkRoot', (Join-Path $OutRoot 'r3'))
Add-Assert 'subset_typo_default.exit_one' ($r3.ExitCode -eq 1) ('exit=' + $r3.ExitCode)
Add-Assert 'subset_typo_default.no_checks' ($r3.Output -match '(?m)^\[gate\] RESULT FAIL no_checks') 'a run with no checks must fail on no_checks'
Add-Assert 'subset_typo_default.cases_zero' ($r3.Output -match '(?m)^\[gate\] cases=0/\d+') 'no case may be recorded as started'

# ---- 12: the opt-out cannot bless an empty run.
Write-Output ''
Write-Output '[test] run 4: the same typo with -AllowPartial'
$r4 = Invoke-GateRun 'subset_typo_partial_allowed' @('-Only', '__no_such_case__', '-AllowPartial', '-WorkRoot', (Join-Path $OutRoot 'r4'))
Add-Assert 'subset_typo_partial_allowed.still_fails' (($r4.ExitCode -eq 1) -and ($r4.Output -match '(?m)^\[gate\] RESULT FAIL no_checks')) ('exit=' + $r4.ExitCode)

# ---- 13-15: a subset run that really executed a case is partial, not PASS.
Write-Output ''
Write-Output '[test] run 5: -Only key_scan_control (a case really runs, 22 of 23 cases do not)'
$r5 = Invoke-GateRun 'subset_healthy_default' @('-Only', 'key_scan_control', '-WorkRoot', (Join-Path $OutRoot 'r5'))
Add-Assert 'subset_healthy_default.exit_one' ($r5.ExitCode -eq 1) ('exit=' + $r5.ExitCode)
Add-Assert 'subset_healthy_default.fails_closed' ($r5.Output -match '(?m)^\[gate\] RESULT FAIL coverage_incomplete') 'an incomplete subset must fail'
Add-Assert 'subset_healthy_default.checks_recorded' (($r5.Output -match '(?m)^covers=key_scan_control\r?$') -and ($r5.Output -match '(?m)^\[gate\] cases=\d+/\d+ checks=[1-9]\d* failed=0')) 'the executed case must still record its checks'
Add-Assert 'subset_healthy_default.no_pass' (-not ($r5.Output -match '(?m)^\[gate\] RESULT PASS ')) 'a subset run must not print RESULT PASS'

# ---- 16-18: the opt-out labels the healthy subset PARTIAL and exits 0.
Write-Output ''
Write-Output '[test] run 6: the same subset with -AllowPartial'
$r6 = Invoke-GateRun 'subset_healthy_partial_allowed' @('-Only', 'key_scan_control', '-AllowPartial', '-WorkRoot', (Join-Path $OutRoot 'r6'))
Add-Assert 'subset_healthy_partial_allowed.exit_zero' ($r6.ExitCode -eq 0) ('exit=' + $r6.ExitCode)
Add-Assert 'subset_healthy_partial_allowed.partial' (($r6.Output -match '(?m)^\[gate\] RESULT PARTIAL ') -and ($r6.Output -match '(?m)^covers=key_scan_control\r?$') -and ($r6.Output -match '(?m)^\[gate\] cases=\d+/\d+ checks=[1-9]\d* failed=0')) 'PARTIAL must carry the executed checks'
Add-Assert 'subset_healthy_partial_allowed.no_pass' (-not ($r6.Output -match '(?m)^\[gate\] RESULT PASS ')) 'a subset run must not print RESULT PASS'

$failed = @($script:Asserts | Where-Object { -not $_.Pass })
Write-Output ''
Write-Output ('[self-test] assertions=' + $script:Asserts.Count + ' failed=' + $failed.Count)
if ($failed.Count -gt 0) {
    foreach ($failure in $failed) {
        Write-Output ('[self-test] FAILED: ' + $failure.Name + ' :: ' + $failure.Detail)
    }
    Write-Output ('[self-test] logs: ' + $OutRoot)
    Write-Output '[self-test] RESULT FAIL'
    exit 1
}
Write-Output ('[self-test] logs: ' + $OutRoot)
if (-not $KeepRoots) {
    try { Remove-Item -LiteralPath $OutRoot -Recurse -Force -ErrorAction Stop }
    catch { Write-Output ('[self-test] cleanup skipped: ' + $_.Exception.Message) }
}
Write-Output '[self-test] RESULT PASS'
exit 0
