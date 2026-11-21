#requires -Version 5.1
[CmdletBinding()]
param(
    # A real 4.7.2 keyed release template. Every run that reaches an export needs it,
    # so this is a workstation test, not a fresh-clone test.
    [string]$KeyedTemplate = '',
    [string]$KeyFile = '',
    # Root for the gate's work roots. Default: out/retention-<timestamp> (gitignored).
    [string]$OutRoot = '',
    # Optional pre-change gate output to compare the check set against, in addition to
    # the in-harness -KeepArtifacts control leg.
    [string]$ControlLog = ''
)

# Closure test for the export-contract gate's scratch retention:
#   * a case's project tree and disposable stage are removed as the run proceeds
#   * the two stages a later case consumes survive
#   * a failing case is pinned, an earlier passing case is still disposed
#   * every release executable is a hardlink to one shared copy
#   * the recorded checks are identical with and without disposal
#   * a passing run's work root is removed by default (-KeepWorkRoot opts out)
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
    $OutRoot = Join-Path $Here ('out\retention-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null
$OutRoot = (Resolve-Path -LiteralPath $OutRoot).Path

if (-not (Test-Path -LiteralPath $KeyedTemplate)) {
    Write-Output ('[test] SKIP prerequisite: keyed release template not found: ' + $KeyedTemplate)
    Write-Output '[test] This harness needs a real keyed template; it cannot run on a fresh clone.'
    exit 2
}
if (-not (Test-Path -LiteralPath $KeyFile)) {
    Write-Output ('[test] SKIP prerequisite: key file not found: ' + $KeyFile)
    exit 2
}

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
    param([string[]]$GateArguments)
    # Native stderr (Godot notices) must not abort the harness under
    # $ErrorActionPreference = 'Stop', so relax it for the child and gate on the code.
    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $raw = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Gate @GateArguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousEap
    }
    if ($null -eq $code) { $code = -1 }
    return [pscustomobject]@{ Output = (($raw | Out-String)); ExitCode = [int]$code }
}

# Only the lines that carry a verdict or a check result, with the work root masked so
# two runs can be compared textually.
function Get-GateCheckText {
    param([string]$Text, [string[]]$PathsToMask = @())
    $kept = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($Text -split "`r?`n")) {
        $masked = $line
        foreach ($path in $PathsToMask) {
            if ([string]::IsNullOrEmpty($path)) { continue }
            $masked = $masked.Replace($path, '<WORK>')
            $masked = $masked.Replace(($path.Replace('\', '/')), '<WORK>')
            $masked = $masked.Replace(($path.Replace('/', '\')), '<WORK>')
        }
        if ($masked -match '^\[(PASS|FAIL|LIMIT)\]') {
            # Compare the tag and the check name only: details carry per-run timings
            # (PACK_VERIFY ms=) and the per-export key id, which are not verdicts.
            $kept.Add(($masked -split ' :: ', 2)[0])
        } elseif ($masked -match '^\[gate\] checks=' -or
            $masked -match '^covers=' -or
            $masked -match '^not_covered=' -or
            $masked -match '^known_limits=' -or
            $masked -match '^\[gate\] RESULT' -or
            $masked -match '^\[gate\] artifacts kept') {
            $kept.Add($masked)
        }
    }
    return ($kept -join "`n")
}

$CommonArguments = @('-KeyedTemplate', $KeyedTemplate, '-KeyFile', $KeyFile)
$PairCases = 'protected_success,unprotected_success'

# ---- run 1: disposal is the default; the work root is kept only so it can be inspected.
$run1Root = Join-Path $OutRoot 'run1'
Write-Output ''
Write-Output '[test] run 1: -Only protected_success,unprotected_success (default disposal, -KeepWorkRoot)'
$run1 = Invoke-Gate (@('-Only', $PairCases, '-WorkRoot', $run1Root, '-KeepWorkRoot') + $CommonArguments)
$run1Out = $run1.Output
[System.IO.File]::WriteAllText((Join-Path $OutRoot 'run1.log'), $run1Out, (New-Object System.Text.UTF8Encoding($false)))

Add-TestCheck 'run1.exit_zero' ($run1.ExitCode -eq 0) ('exit=' + $run1.ExitCode)
Add-TestCheck 'run1.result_pass' ($run1Out -match '(?m)^\[gate\] RESULT PASS ') 'RESULT PASS must be printed'
Add-TestCheck 'run1.both_cases_covered' (($run1Out -match '(?m)^covers=.*protected_success') -and ($run1Out -match '(?m)^covers=.*unprotected_success')) 'covers= must name both cases'
Add-TestCheck 'run1.checks_recorded' ($run1Out -match '(?m)^\[gate\] checks=[1-9]\d* failed=0\r?$') 'checks= must be non-zero with no failures'
$run1CheckCountMatch = [regex]::Match($run1Out, '(?m)^\[gate\] checks=(\d+) failed=(\d+)\r?$')
$run1CheckCount = -1
if ($run1CheckCountMatch.Success) { $run1CheckCount = [int]$run1CheckCountMatch.Groups[1].Value }

# ---- assertions 2 and 3: retained stages survive, case projects are disposed.
$retainedProtected = Join-Path $run1Root 'stage-protected_success'
$retainedUnprotected = Join-Path $run1Root 'stage-unprotected_success'
Add-TestCheck 'run1.retained_stages_exist' ((Test-Path -LiteralPath $retainedProtected) -and (Test-Path -LiteralPath $retainedUnprotected)) ('protected=' + (Test-Path -LiteralPath $retainedProtected) + ' unprotected=' + (Test-Path -LiteralPath $retainedUnprotected))
$caseProtected1 = Join-Path $run1Root 'case-protected_success'
$caseUnprotected1 = Join-Path $run1Root 'case-unprotected_success'
Add-TestCheck 'run1.case_projects_disposed' ((-not (Test-Path -LiteralPath $caseProtected1)) -and (-not (Test-Path -LiteralPath $caseUnprotected1))) ('protected_exists=' + (Test-Path -LiteralPath $caseProtected1) + ' unprotected_exists=' + (Test-Path -LiteralPath $caseUnprotected1))

# ---- run 4: the escape hatch keeps case projects; also the in-harness control leg.
$run4Root = Join-Path $OutRoot 'run4'
Write-Output ''
Write-Output '[test] run 4: same case set with -KeepArtifacts (control leg for the check set)'
$run4 = Invoke-Gate (@('-Only', $PairCases, '-WorkRoot', $run4Root, '-KeepArtifacts') + $CommonArguments)
$run4Out = $run4.Output
[System.IO.File]::WriteAllText((Join-Path $OutRoot 'run4.log'), $run4Out, (New-Object System.Text.UTF8Encoding($false)))

Add-TestCheck 'run4.exit_zero' ($run4.ExitCode -eq 0) ('exit=' + $run4.ExitCode)
Add-TestCheck 'run4.case_projects_kept' ((Test-Path -LiteralPath (Join-Path $run4Root 'case-protected_success')) -and (Test-Path -LiteralPath (Join-Path $run4Root 'case-unprotected_success'))) 'both case directories must exist with -KeepArtifacts'

# ---- assertion 7: one shared copy, and every release executable is a hardlink.
$sharedExe = Join-Path $run4Root '_shared\game.exe'
$sharedExists = Test-Path -LiteralPath $sharedExe
Add-TestCheck 'run4.shared_executable_exists' $sharedExists ('path=' + $sharedExe)
$linkType = ''
$releaseExe = Join-Path $run4Root 'case-protected_success\release\game.exe'
if (Test-Path -LiteralPath $releaseExe) {
    $linkType = [string](Get-Item -LiteralPath $releaseExe).LinkType
}
Add-TestCheck 'run4.release_executable_is_hardlink' ($sharedExists -and ($linkType -eq 'HardLink')) ('link_type=' + $linkType + ' (file is ' + (Test-Path -LiteralPath $releaseExe) + ')')

# ---- run 5: a failing case is pinned, the earlier passing case is still disposed.
$run5Root = Join-Path $OutRoot 'run5'
Write-Output ''
Write-Output '[test] run 5: -NegativeControl -Only protected_success,alert_bypass'
$run5 = Invoke-Gate (@('-NegativeControl', '-Only', 'protected_success,alert_bypass', '-WorkRoot', $run5Root) + $CommonArguments)
$run5Out = $run5.Output
[System.IO.File]::WriteAllText((Join-Path $OutRoot 'run5.log'), $run5Out, (New-Object System.Text.UTF8Encoding($false)))

Add-TestCheck 'run5.exit_nonzero' ($run5.ExitCode -eq 1) ('exit=' + $run5.ExitCode)
Add-TestCheck 'run5.failing_case_pinned' ($run5Out -match '(?m)^\[gate\] artifacts kept for failing case: alert_bypass\r?$') 'the pin line must name alert_bypass'
# alert_bypass only ever creates a stage, never a case project.
Add-TestCheck 'run5.failing_artifacts_exist' (Test-Path -LiteralPath (Join-Path $run5Root 'stage-alert_bypass')) ('stage_exists=' + (Test-Path -LiteralPath (Join-Path $run5Root 'stage-alert_bypass')))
Add-TestCheck 'run5.passing_case_disposed' (-not (Test-Path -LiteralPath (Join-Path $run5Root 'case-protected_success'))) ('case_exists=' + (Test-Path -LiteralPath (Join-Path $run5Root 'case-protected_success')))

# ---- run 6: a passing run removes its work root by default.
$run6Root = Join-Path $OutRoot 'run6'
Write-Output ''
Write-Output '[test] run 6: -Only protected_success with no keep switch (work root must be removed)'
$run6 = Invoke-Gate (@('-Only', 'protected_success', '-WorkRoot', $run6Root) + $CommonArguments)
$run6Out = $run6.Output

Add-TestCheck 'run6.exit_zero' ($run6.ExitCode -eq 0) ('exit=' + $run6.ExitCode)
Add-TestCheck 'run6.cleanup_announced' ($run6Out -match '(?m)^\[gate\] cleanup: removing ') 'the cleanup line must name the work root'
Add-TestCheck 'run6.work_root_removed' (-not (Test-Path -LiteralPath $run6Root)) ('root_exists=' + (Test-Path -LiteralPath $run6Root))

# ---- assertion 8: disposal changed no check result.
$run1Text = Get-GateCheckText -Text $run1Out -PathsToMask @($run1Root)
$run4Text = Get-GateCheckText -Text $run4Out -PathsToMask @($run4Root)
Add-TestCheck 'run1.check_set_matches_keep_artifacts_control' ($run1Text -eq $run4Text) 'the [PASS]/[FAIL]/footer set must be identical with disposal on and off'
if (-not [string]::IsNullOrEmpty($ControlLog)) {
    if (Test-Path -LiteralPath $ControlLog) {
        $controlText = [System.IO.File]::ReadAllText($ControlLog)
        $normalisedControl = Get-GateCheckText -Text $controlText -PathsToMask @($run1Root)
        Add-TestCheck 'run1.check_set_matches_pre_change_capture' ($run1Text -eq $normalisedControl) ('control=' + $ControlLog)
    } else {
        Add-TestCheck 'run1.check_set_matches_pre_change_capture' $false ('control log missing: ' + $ControlLog)
    }
}

$failed = @($script:Checks | Where-Object { -not $_.Pass })
Write-Output ''
Write-Output ('[test] assertions=' + $script:Checks.Count + ' failed=' + $failed.Count + ' checks_recorded=' + $run1CheckCount)
Write-Output ('[test] work roots: ' + (Join-Path $OutRoot 'run1') + ' ; ' + $run4Root + ' ; ' + $run5Root)
if ($failed.Count -gt 0) {
    foreach ($failure in $failed) {
        Write-Output ('[test] FAILED: ' + $failure.Name + ' :: ' + $failure.Detail)
    }
    Write-Output '[test] RESULT FAIL'
    exit 1
}
Write-Output '[test] RESULT PASS'
exit 0
