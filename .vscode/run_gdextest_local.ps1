<#
.SYNOPSIS
    Device A local test runner: replicates the `gdextest test` post-build
    stages (doctor, fixture, warmup, run, report) without invoking the CLI.

.DESCRIPTION
    The `gdextest` CLI always builds locally and has no `--no-build` flag,
    so it is never invoked here: Device B already compiled the test library
    (see .vscode/build_remote.ps1 -IncludeTests, which pulls the result back
    to ./bin/) and this script runs the remaining stages from the repo root,
    stopping at the first failure (banner style mirrors build_remote.ps1).

    Stages:
      [1/5] doctor  - `gdextest doctor` (config/Godot/SCons checks, no build).
      [2/5] fixture - `generate_fixture.py` with the newest
                      bin/libdirectstorage_tests*.dll, then manual staging of
                      `project/test_assets/**/*` per [gdextest.fixture] assets.
      [3/5] warmup  - cold-cache deterministic pass (exit 134 tolerated).
      [4/5] run     - headless editor run; renderer flag BEFORE `--`,
                      runner flags AFTER `--`, and nothing else after `--`.
      [5/5] report  - `gdextest report` console + JUnit; its exit code is the
                      script exit code (test failures still reach report).

    NOTE (fixture assets): generate_fixture.py main() exposes no
    --fixture-asset flag, and this call passes none, so the generator's
    project_source_root copy step is a no-op (verified in main(): it passes
    Path.cwd() but fixture_assets defaults to []). Assets are therefore
    copied manually with the same layout the CLI would produce
    (<fixture>/project/test_assets/...); the staged count is printed and a
    missing landing is reported as a fixture failure.

    CAVEAT (consumer_extension): generate_fixture.py main() has NO
    extension-library flags (only the generate_fixture() function supports
    extension_library/extension_manifest). If a later spike decision picks
    `consumer_extension` over compile-in, staging lines for the consumer
    extension library + manifest must be added to stage [2/5] here.
    Out of scope for this task.
#>
param(
    [string]$Filter = "",
    [string]$RenderingDriver = "",
    [string]$Json = "results.json",
    [string]$GodotExe = ($env:GODOT),
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"

function Show-StepFailure {
    param(
        [string]$Step,
        [string]$Message,
        [int]$Code = 1
    )
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host " [RUN FAILED] $Step " -ForegroundColor White -BackgroundColor DarkRed
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host " Reason: $Message" -ForegroundColor Red
    if ($Code -ne 0) {
        Write-Host " Exit Code: $Code" -ForegroundColor Yellow
    }
    Write-Host " Check the terminal output above for details." -ForegroundColor DarkGray
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host ""
    exit $Code
}

function Get-GodotConsolePath {
    param(
        [string]$RepoRoot
    )
    if ($env:GODOT_CONSOLE_PATH) {
        return $env:GODOT_CONSOLE_PATH
    }
    $candidates = @(
        (Join-Path $RepoRoot ".env"),
        (Join-Path $PSScriptRoot "build_remote.env")
    )
    foreach ($path in $candidates) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        foreach ($line in (Get-Content -LiteralPath $path)) {
            $trimmed = $line.Trim()
            if (-not $trimmed -or $trimmed.StartsWith("#")) { continue }
            $parts = $trimmed -split '=', 2
            if ($parts.Count -eq 2 -and $parts[0].Trim() -eq "GODOT_CONSOLE_PATH") {
                $value = $parts[1].Trim().Trim('"').Trim("'")
                if ($value) { return $value }
            }
        }
    }
    return $null
}

function Show-RunSuccess {
    param(
        [string]$Message = "All test stages completed successfully!"
    )
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Green
    Write-Host " [RUN SUCCESS] $Message " -ForegroundColor White -BackgroundColor DarkGreen
    Write-Host "================================================================================" -ForegroundColor Green
    Write-Host ""
}

function Write-VerboseHost {
    param(
        [string]$Message,
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray
    )
    if ($Verbose) {
        Write-Host $Message -ForegroundColor $ForegroundColor
    }
}

# All stages run from the repo root (this script lives in .vscode/).
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $RepoRoot

if (-not $GodotExe) {
    $GodotExe = Get-GodotConsolePath -RepoRoot $RepoRoot
}
if (-not $GodotExe) {
    Show-StepFailure -Step "Configuration Error" -Message "No Godot binary: pass -GodotExe, set GODOT, or set GODOT_CONSOLE_PATH in .env." -Code 1
}

$FixtureRoot = "build/gdextest/project"

# [1/5] doctor: validates config/Godot/SCons without building.
# NOTE: invoke via `python .../gdextest.py`, never `& ./extern/gdextest/gdextest`
# (a POSIX sh wrapper that cannot activate on Windows PowerShell; spike
# defect #2). $LASTEXITCODE may stay $null when activation fails, so coerce.
Write-VerboseHost "[1/5] doctor: validating config/Godot/SCons (no build)..." -ForegroundColor Cyan
Write-VerboseHost "Using Godot: $GodotExe" -ForegroundColor DarkGray
$env:GODOT = $GodotExe
if ($Verbose) {
    & python extern/gdextest/tools/gdextest.py doctor
} else {
    $doctorOut = & python extern/gdextest/tools/gdextest.py doctor 2>&1
}
$DoctorCode = $LASTEXITCODE
if ((-not $?) -or ($null -eq $DoctorCode) -or ($DoctorCode -ne 0)) {
    if (-not $Verbose -and $doctorOut) {
        Write-Host ($doctorOut -join "`n") -ForegroundColor Red
    }
    if ($null -eq $DoctorCode -or $DoctorCode -eq 0) { $DoctorCode = 1 }
    Show-StepFailure -Step "Step [1/5] Doctor" -Message "gdextest doctor checks failed." -Code $DoctorCode
}

# [2/5] fixture: generate the disposable Godot project for the test library.
Write-VerboseHost "[2/5] fixture: generating Godot fixture project..." -ForegroundColor Cyan
$TestDll = Get-ChildItem -Path "./bin/libdirectstorage_tests*.dll" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $TestDll) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "No test library found at bin/libdirectstorage_tests*.dll. Build with .vscode/build_remote.ps1 -IncludeTests first." -Code 1
}
Write-VerboseHost "Using test library: $($TestDll.FullName)" -ForegroundColor DarkGray
if ($Verbose) {
    & python extern/gdextest/tools/generate_fixture.py --project-root $FixtureRoot --library $TestDll.FullName --godot-version 4.7 --library-key windows.debug.x86_64 --project-name "directstorage-accelerator-tests" --scan-timeout-ms 30000
} else {
    $fixtureOut = & python extern/gdextest/tools/generate_fixture.py --project-root $FixtureRoot --library $TestDll.FullName --godot-version 4.7 --library-key windows.debug.x86_64 --project-name "directstorage-accelerator-tests" --scan-timeout-ms 30000 2>&1
}
if ($LASTEXITCODE -ne 0) {
    if (-not $Verbose -and $fixtureOut) {
        Write-Host ($fixtureOut -join "`n") -ForegroundColor Red
    }
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "generate_fixture.py failed." -Code $LASTEXITCODE
}

# [2/5] fixture assets: the generator call above stages none (no
# --fixture-asset flags), so copy per [gdextest.fixture] assets manually,
# preserving repo-relative paths exactly as copy_globbed_assets would.
Write-VerboseHost "Staging [gdextest.fixture] assets (project/test_assets/**/*)..." -ForegroundColor Cyan
$AssetSource = Join-Path $RepoRoot "project/test_assets"
if (-not (Test-Path $AssetSource)) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "Asset source project/test_assets/ not found; cannot stage fixture assets." -Code 1
}
$AssetFiles = Get-ChildItem -Path $AssetSource -Recurse -File
$stagedCount = 0
foreach ($file in $AssetFiles) {
    $relative = $file.FullName.Substring($RepoRoot.Length).TrimStart("\", "/")
    $destination = Join-Path (Join-Path $RepoRoot $FixtureRoot) $relative
    $destinationParent = Split-Path $destination -Parent
    if (-not (Test-Path $destinationParent)) {
        New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
    }
    Copy-Item -Path $file.FullName -Destination $destination -Force
    $stagedCount++
}
$FixtureAssets = Join-Path (Join-Path $RepoRoot $FixtureRoot) "project/test_assets"
if (-not (Test-Path $FixtureAssets)) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "Fixture assets did not land in $FixtureRoot/project/test_assets/." -Code 1
}
Write-VerboseHost "Fixture assets staged: $stagedCount file(s) -> $FixtureRoot/project/test_assets/" -ForegroundColor Green

# [3/5] warmup: cold-cache deterministic pass (output discarded).
# A single retry absorbs the observed cold-cache flake (first attempt once
# exited 0xC0000005, immediate retry passed; spike observation). Exit 134
# (cold-cache shutdown abort) is expected per upstream docs/cli.md.
Write-VerboseHost "[3/5] warmup: cold-cache deterministic pass..." -ForegroundColor Cyan
# Godot writes warnings to stderr; with $ErrorActionPreference = "Stop" that
# becomes a terminating NativeCommandError before $LASTEXITCODE is read.
# Relax EAP for the native call and gate purely on the exit code.
$WarmupCode = 1
for ($attempt = 1; $attempt -le 2; $attempt++) {
    $ErrorActionPreference = "Continue"
    & $GodotExe --headless --editor --path $FixtureRoot --quit-after 2 *> $null
    $WarmupCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($null -eq $WarmupCode) { $WarmupCode = 1 }
    if ($WarmupCode -eq 0 -or $WarmupCode -eq 134) { break }
    Write-VerboseHost "Warmup attempt $attempt exited $WarmupCode; retrying once..." -ForegroundColor Yellow
}
if ($WarmupCode -eq 134) {
    Write-VerboseHost "Warmup exited 134 (cold-cache shutdown abort; import completed -- expected per upstream docs/cli.md)." -ForegroundColor Yellow
} elseif ($WarmupCode -ne 0) {
    Show-StepFailure -Step "Step [3/5] Warmup" -Message "Warmup pass failed twice." -Code $WarmupCode
}

# [4/5] run: renderer flag BEFORE `--`, runner flags AFTER `--`,
# and never anything else after `--`. Test failures (exit 1) still proceed
# to report; the report exit code gates the pipeline.
Write-VerboseHost "[4/5] run: headless editor test run..." -ForegroundColor Cyan
$GodotArgs = @("--headless", "--editor")
if ($RenderingDriver) {
    $GodotArgs += "--rendering-driver"
    $GodotArgs += $RenderingDriver
}
$GodotArgs += "--path"
$GodotArgs += $FixtureRoot
$GodotArgs += "--"
$GodotArgs += "--gdextest-run"
if ($Filter) {
    $GodotArgs += "--gdextest-filter=$Filter"
}
# The runner flag is resolved by Godot against --path, so a relative $Json
# lands inside the fixture dir; anchor the report stage to the same file
# (spike defect #5: `report results.json` from repo root found nothing).
$ReportJson = if ([System.IO.Path]::IsPathRooted($Json)) { $Json } else { Join-Path $FixtureRoot $Json }
$GodotArgs += "--gdextest-json=$Json"
Write-VerboseHost "Godot command: $GodotExe $($GodotArgs -join ' ')" -ForegroundColor DarkGray
# Remove any previous results so a crashed run cannot be masked by a stale
# JSON (a prior crash left an old results.json that the report stage happily
# re-printed as PASSED).
if (Test-Path $ReportJson) {
    Remove-Item -Force -Path $ReportJson -ErrorAction SilentlyContinue
}
# Same EAP relaxation as the warmup: engine warnings on stderr must not abort
# the run before the JSON is written.
$ErrorActionPreference = "Continue"
if ($Verbose) {
    & $GodotExe @GodotArgs
} else {
    $godotOut = & $GodotExe @GodotArgs 2>&1
}
$RunCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($null -eq $RunCode) { $RunCode = 1 }
# Exit 0 = all passed, 1 = test failures (both still write JSON). A negative
# code (e.g. 0xC0000005-class abort) means the host crashed before writing.
if ($RunCode -lt 0) {
    if (-not $Verbose -and $godotOut) {
        Write-Host "--- Godot Engine Output (tail) ---" -ForegroundColor Red
        $godotOut | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    }
    Show-StepFailure -Step "Step [4/5] Run" -Message "Godot host crashed before writing results (exit $RunCode). See the engine output above." -Code 1
}
if (-not (Test-Path $ReportJson)) {
    if (-not $Verbose -and $godotOut) {
        Write-Host "--- Godot Engine Output (tail) ---" -ForegroundColor Red
        $godotOut | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    }
    Show-StepFailure -Step "Step [4/5] Run" -Message "Test run produced no results file at $ReportJson (exit $RunCode); the suite likely aborted during startup." -Code $(if ($RunCode -ne 0) { $RunCode } else { 1 })
}
if ($RunCode -ne 0) {
    Write-VerboseHost "Test run exited $RunCode; proceeding to report (report exit code gates the pipeline)." -ForegroundColor Yellow
}

# [5/5] report: console + JUnit; propagate its exit code as the script's.
# Same python-invocation + null-code-coercion rules as step [1/5].
Write-VerboseHost "[5/5] report: rendering console + JUnit report..." -ForegroundColor Cyan
if ($Verbose) {
    & python extern/gdextest/tools/gdextest.py report $ReportJson --console --json $ReportJson --junit=test-results.xml
} else {
    $reportOut = & python extern/gdextest/tools/gdextest.py report $ReportJson --console --json $ReportJson --junit=test-results.xml 2>&1
}
$ReportCode = $LASTEXITCODE
if ((-not $?) -or ($null -eq $ReportCode)) { $ReportCode = 1 }
# Concise totals line: the gtest-style console report buries pass/fail/skip
# in per-test lines, so print one summary from the merged JSON (JSON was
# written back to $ReportJson, not stdout).
try {
    $doc = Get-Content -LiteralPath $ReportJson -Raw | ConvertFrom-Json
    $t = $doc.totals
    if ($t) {
        $crashedNote = if ($t.crashed) { ", crashed: $($t.crashed)" } else { "" }
        $color = if ($ReportCode -eq 0) { "Green" } else { "Red" }
        Write-Host "Tests: passed $($t.pass), failed $($t.fail), skipped $($t.skip)$crashedNote" -ForegroundColor $color
    }
} catch {
    Write-Host "Tests: could not parse totals from $ReportJson" -ForegroundColor Yellow
}
if ($ReportCode -ne 0) {
    if (-not $Verbose -and $reportOut) {
        Write-Host "--- Test Failure Details ---" -ForegroundColor Red
        $reportOut | ForEach-Object { Write-Host $_ }
    }
    Show-StepFailure -Step "Step [5/5] Report" -Message "Tests failed or report could not be generated (see console report above)." -Code $ReportCode
}

Show-RunSuccess -Message "All test stages completed successfully!"
exit 0
