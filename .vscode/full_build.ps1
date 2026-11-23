<#
.SYNOPSIS
    Full Build Runner: compiles all templates (Release + Debug + Tests) and runs the test suite,
    reporting individual stage timings and the total combined execution time.
#>
param(
    [switch]$Verbose,
    [string]$Filter = "",
    [switch]$SkipVerify,
    [switch]$SkipGut,
    [switch]$Local,
    [switch]$Remote
)

$ErrorActionPreference = "Stop"
$MasterStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Show-FullBuildBanner {
    param(
        [bool]$Success,
        [string]$StageFailed = "",
        [TimeSpan]$BuildDuration = [TimeSpan]::Zero,
        [TimeSpan]$TestDuration = [TimeSpan]::Zero,
        [TimeSpan]$TotalDuration = [TimeSpan]::Zero
    )
    Write-Host ""
    $borderColor = if ($Success) { [ConsoleColor]::Green } else { [ConsoleColor]::Red }
    Write-Host "================================================================================" -ForegroundColor $borderColor
    if ($Success) {
        Write-Host " [FULL BUILD SUCCESS] All stages completed successfully! " -ForegroundColor White -BackgroundColor DarkGreen
    } else {
        Write-Host " [FULL BUILD FAILED] Failed at stage: $StageFailed " -ForegroundColor White -BackgroundColor DarkRed
    }
    Write-Host "================================================================================" -ForegroundColor $borderColor

    $formatSpan = {
        param([TimeSpan]$ts)
        if ($ts.TotalMinutes -ge 1) {
            $mins = [int][Math]::Floor($ts.TotalMinutes)
            return ("{0}m {1:D2}s ({2:F1}s total)" -f $mins, $ts.Seconds, $ts.TotalSeconds)
        }
        return ("{0:F1}s" -f $ts.TotalSeconds)
    }

    $bStr = & $formatSpan $BuildDuration
    $tStr = & $formatSpan $TestDuration
    $totStr = & $formatSpan $TotalDuration

    if ($BuildDuration -gt [TimeSpan]::Zero -and $TestDuration -gt [TimeSpan]::Zero) {
        Write-Host " Stage Timings       : Build: $bStr | Tests: $tStr" -ForegroundColor DarkGray
    }
    Write-Host " Total Combined Time : $totStr" -ForegroundColor $(if ($Success) { [ConsoleColor]::Green } else { [ConsoleColor]::Yellow })
    Write-Host "================================================================================" -ForegroundColor $borderColor
    Write-Host ""
}

# --- Stage 1: Build Templates (Release + Debug + Tests) ---
$buildScript = Join-Path $PSScriptRoot "build_remote.ps1"
if (-not (Test-Path -LiteralPath $buildScript)) {
    Write-Host "Error: Build script not found at $buildScript" -ForegroundColor Red
    exit 1
}

$buildArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $buildScript,
    "-Target", "template_debug_and_release",
    "-IncludeTests"
)
if ($Verbose) { $buildArgs += "-Verbose" }
if ($SkipVerify) { $buildArgs += "-SkipVerify" }
if ($Local) { $buildArgs += "-Local" }
if ($Remote) { $buildArgs += "-Remote" }

$buildSw = [System.Diagnostics.Stopwatch]::StartNew()
& powershell @buildArgs
$buildExit = $LASTEXITCODE
$buildSw.Stop()

if ($buildExit -ne 0) {
    $MasterStopwatch.Stop()
    Show-FullBuildBanner -Success $false -StageFailed "Compilation & Artifact Sync" -BuildDuration $buildSw.Elapsed -TotalDuration $MasterStopwatch.Elapsed
    exit $buildExit
}

# --- Stage 2: Run Tests (Headless) ---
$testScript = Join-Path $PSScriptRoot "run_gdextest_local.ps1"
if (-not (Test-Path -LiteralPath $testScript)) {
    Write-Host "Error: Test runner script not found at $testScript" -ForegroundColor Red
    exit 1
}

$testArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $testScript
)
if ($Verbose) { $testArgs += "-Verbose" }
if ($Filter) { $testArgs += @("-Filter", $Filter) }

$testSw = [System.Diagnostics.Stopwatch]::StartNew()
& powershell @testArgs
$testExit = $LASTEXITCODE
$testSw.Stop()
if ($testExit -ne 0) {
    $MasterStopwatch.Stop()
    Show-FullBuildBanner -Success $false -StageFailed "Test Suite Execution" -BuildDuration $buildSw.Elapsed -TestDuration $testSw.Elapsed -TotalDuration $MasterStopwatch.Elapsed
    exit $testExit
}

# --- Stage 3: Run GDScript GUT Unit Tests ---
if (-not $SkipGut) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $gutRunner = Join-Path $repoRoot "tools\run_gut.py"
    if (-not (Test-Path -LiteralPath $gutRunner)) {
        $MasterStopwatch.Stop()
        Write-Host "Error: GUT runner script not found at $gutRunner" -ForegroundColor Red
        Show-FullBuildBanner -Success $false -StageFailed "GDScript GUT Suite" -BuildDuration $buildSw.Elapsed -TestDuration $testSw.Elapsed -TotalDuration $MasterStopwatch.Elapsed
        exit 1
    }
    $gutSw = [System.Diagnostics.Stopwatch]::StartNew()
    & python $gutRunner
    $gutExit = $LASTEXITCODE
    $gutSw.Stop()
    if ($gutExit -ne 0) {
        $MasterStopwatch.Stop()
        Show-FullBuildBanner -Success $false -StageFailed "GDScript GUT Suite" -BuildDuration $buildSw.Elapsed -TestDuration ($testSw.Elapsed + $gutSw.Elapsed) -TotalDuration $MasterStopwatch.Elapsed
        exit $gutExit
    }
    $testSwDuration = $testSw.Elapsed + $gutSw.Elapsed
} else {
    $testSwDuration = $testSw.Elapsed
}

$MasterStopwatch.Stop()
Show-FullBuildBanner -Success $true -BuildDuration $buildSw.Elapsed -TestDuration $testSwDuration -TotalDuration $MasterStopwatch.Elapsed
exit 0
