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

# Don't use verbose unless an error happened and it requires inspecting
# every build process' output.

param(
    [string]$Filter = "",
    [string]$RenderingDriver = "",
    [string]$Json = "results.json",
    [string]$GodotExe = ($env:GODOT),
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"

function Write-ReportText {
    param(
        [string]$Message = "",
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray,
        $BackgroundColor = $null,
        [switch]$IsError
    )
    if ([Console]::IsOutputRedirected -or [Console]::IsErrorRedirected) {
        if ($IsError) {
            [Console]::Error.WriteLine($Message)
        } else {
            [Console]::Out.WriteLine($Message)
        }
    } else {
        if ($null -ne $BackgroundColor) {
            Write-Host $Message -ForegroundColor $ForegroundColor -BackgroundColor $BackgroundColor
        } else {
            Write-Host $Message -ForegroundColor $ForegroundColor
        }
    }
}

function Show-StepFailure {
    param(
        [string]$Step,
        [string]$Message,
        [int]$Code = 1
    )
    Write-ReportText "" -IsError
    Write-ReportText "================================================================================" -ForegroundColor Red -IsError
    Write-ReportText " [RUN FAILED] $Step " -ForegroundColor White -BackgroundColor DarkRed -IsError
    Write-ReportText "================================================================================" -ForegroundColor Red -IsError
    Write-ReportText " Reason: $Message" -ForegroundColor Red -IsError
    if ($Code -ne 0) {
        Write-ReportText " Exit Code: $Code" -ForegroundColor Yellow -IsError
    }
    Write-ReportText " Check the terminal output above for details." -ForegroundColor DarkGray -IsError
    Write-ReportText "================================================================================" -ForegroundColor Red -IsError
    Write-ReportText "" -IsError
    exit $Code
}

function Stop-OrphanFixtureProcess {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [string]$FixturePath
    )
    try {
        $godotProcs = Get-Process -Name "*godot*" -ErrorAction SilentlyContinue
        if ($godotProcs) {
            $resolved = ""
            if (Test-Path -LiteralPath $FixturePath) {
                $resolved = (Resolve-Path -LiteralPath $FixturePath -ErrorAction SilentlyContinue).Path
            }
            foreach ($p in $godotProcs) {
                $match = $false
                try {
                    $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)" -ErrorAction SilentlyContinue
                    if ($cim -and $cim.CommandLine) {
                        $cmd = $cim.CommandLine
                        if ($resolved -and $cmd.IndexOf($resolved, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                            $match = $true
                        } elseif ($cmd -match "build[/\\]gdextest[/\\]project" -or $cmd -match "--gdextest-run") {
                            $match = $true
                        }
                    }
                } catch {
                    $null = $_
                }
                if ($match) {
                    Write-ReportText "Terminating orphan Godot fixture process (PID $($p.Id))..." -ForegroundColor Yellow
                    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                    Start-Sleep -Milliseconds 250
                }
            }
        }
    } catch {
        $null = $_
    }

    # Clean up stale locked shadow DLLs if still remaining in fixture addons bin
    $binDir = Join-Path $FixturePath "addons/gdextest/bin"
    if (Test-Path -LiteralPath $binDir) {
        $staleDlls = Get-ChildItem -Path $binDir -Filter "~*.dll" -ErrorAction SilentlyContinue
        foreach ($stale in $staleDlls) {
            try {
                Remove-Item -LiteralPath $stale.FullName -Force -ErrorAction Stop
            } catch {
                # Stale DLL is still locked; re-check only matching fixture processes before retrying
                $resolved = ""
                if (Test-Path -LiteralPath $FixturePath) {
                    $resolved = (Resolve-Path -LiteralPath $FixturePath -ErrorAction SilentlyContinue).Path
                }
                $remainingProcs = Get-Process -Name "*godot*" -ErrorAction SilentlyContinue
                foreach ($p in $remainingProcs) {
                    try {
                        $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)" -ErrorAction SilentlyContinue
                        if ($cim -and $cim.CommandLine) {
                            $cmd = $cim.CommandLine
                            if (($resolved -and $cmd.IndexOf($resolved, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) -or
                                ($cmd -match "build[/\\]gdextest[/\\]project" -or $cmd -match "--gdextest-run")) {
                                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                            }
                        }
                    } catch {
                        $null = $_
                    }
                }
                Start-Sleep -Milliseconds 300
                Remove-Item -LiteralPath $stale.FullName -Force -ErrorAction SilentlyContinue
            }
        }
    }
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
    Write-ReportText ""
    Write-ReportText "================================================================================" -ForegroundColor Green
    Write-ReportText " [RUN SUCCESS] $Message " -ForegroundColor White -BackgroundColor DarkGreen
    Write-ReportText "================================================================================" -ForegroundColor Green
    Write-ReportText ""
}

function Write-VerboseHost {
    param(
        [string]$Message,
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray
    )
    if ($Verbose) {
        Write-ReportText $Message -ForegroundColor $ForegroundColor
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

# Ensure the gdextest submodule is present. A plain `git clone` without
# --recursive leaves extern/gdextest empty and every stage below fails with
# a confusing python "can't open file" error. Init exactly the pinned commit
# (never pull latest) and fail clearly if that is impossible.
$GdextestCli = Join-Path $RepoRoot "extern/gdextest/tools/gdextest.py"
if (-not (Test-Path -LiteralPath $GdextestCli)) {
    Write-ReportText "gdextest submodule is missing; running 'git submodule update --init -- extern/gdextest'..." -ForegroundColor Yellow
    try {
        $subOut = & git submodule update --init -- extern/gdextest 2>&1
        if ($subOut) {
            Write-VerboseHost ($subOut -join "`n") -ForegroundColor DarkGray
        }
    }
    catch {
        $null = $_
    }
    if (-not (Test-Path -LiteralPath $GdextestCli)) {
        Show-StepFailure -Step "Configuration Error" -Message "extern/gdextest is missing and auto-init failed. Run 'git submodule update --init -- extern/gdextest' manually (requires git and network access)." -Code 1
    }
    Write-VerboseHost "gdextest submodule initialized." -ForegroundColor Green
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
        foreach ($line in $doctorOut) {
            Write-ReportText $line -ForegroundColor Red -IsError
        }
    }
    if ($null -eq $DoctorCode -or $DoctorCode -eq 0) { $DoctorCode = 1 }
    Show-StepFailure -Step "Step [1/5] Doctor" -Message "gdextest doctor checks failed." -Code $DoctorCode
}

# [2/5] fixture: generate the disposable Godot project for the test library.
Stop-OrphanFixtureProcess -FixturePath $FixtureRoot
Write-VerboseHost "[2/5] fixture: generating Godot fixture project..." -ForegroundColor Cyan
$TestDll = Get-ChildItem -Path "./bin/libdirectstorage_tests*.editor*.dll" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $TestDll) {
    $TestDll = Get-ChildItem -Path "./bin/libdirectstorage_tests*.dll" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
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
        foreach ($line in $fixtureOut) {
            Write-ReportText $line -ForegroundColor Red -IsError
        }
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

# [2/5] DirectStorage runtime DLLs: stage dstorage*.dll beside the fixture's
# addon copy so Win32Backend::probe_system_support() discovers them via
# res://addons/directstorage_accelerator/bin.
Write-VerboseHost "Staging DirectStorage runtime DLLs into fixture..." -ForegroundColor Cyan
$RuntimeSrcDir = Join-Path $RepoRoot "project/addons/directstorage_accelerator/bin"
$FixtureAddonBin = Join-Path (Join-Path $RepoRoot $FixtureRoot) "addons/directstorage_accelerator/bin"
$RuntimeDlls = @(Get-ChildItem -Path $RuntimeSrcDir -Filter "dstorage*.dll" -File -ErrorAction SilentlyContinue)
if ($RuntimeDlls.Count -gt 0) {
    if (-not (Test-Path -LiteralPath $FixtureAddonBin)) {
        New-Item -ItemType Directory -Force -Path $FixtureAddonBin | Out-Null
    }
    foreach ($runtimeDll in $RuntimeDlls) {
        Copy-Item -LiteralPath $runtimeDll.FullName -Destination (Join-Path $FixtureAddonBin $runtimeDll.Name) -Force
    }
    Write-VerboseHost "DirectStorage runtime staged: $($RuntimeDlls.Count) DLL(s) -> $FixtureRoot/addons/directstorage_accelerator/bin/" -ForegroundColor Green
} else {
    Write-VerboseHost "No dstorage*.dll found in $RuntimeSrcDir; D3D12 GPU suites will skip." -ForegroundColor Yellow
}

# [3/5] warmup: cold-cache deterministic pass (output discarded).
# A single retry absorbs the observed cold-cache flake (first attempt once
# exited 0xC0000005, immediate retry passed; spike observation). Exit 134
# (cold-cache shutdown abort) is expected per upstream docs/cli.md.
Write-VerboseHost "[3/5] warmup: cold-cache deterministic pass..." -ForegroundColor Cyan
# Godot writes warnings to stderr; with $ErrorActionPreference = "Stop" that
# becomes a terminating NativeCommandError before $LASTEXITCODE is read.
# Relax EAP for the native call and gate purely on the exit code.
$WarmupArgs = @("--headless", "--editor")
if ($RenderingDriver) {
    $WarmupArgs += "--rendering-driver"
    $WarmupArgs += $RenderingDriver
}
$WarmupArgs += "--path"
$WarmupArgs += $FixtureRoot
$WarmupArgs += "--quit-after"
$WarmupArgs += "2"

$WarmupCode = 1
for ($attempt = 1; $attempt -le 2; $attempt++) {
    $ErrorActionPreference = "Continue"
    & $GodotExe @WarmupArgs *> $null
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
Stop-OrphanFixtureProcess -FixturePath $FixtureRoot

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
        Write-ReportText "--- Godot Engine Output (tail) ---" -ForegroundColor Red -IsError
        $godotOut | Select-Object -Last 30 | ForEach-Object { Write-ReportText $_ -ForegroundColor Red -IsError }
    }
    Show-StepFailure -Step "Step [4/5] Run" -Message "Godot host crashed before writing results (exit $RunCode). See the engine output above." -Code 1
}
if (-not (Test-Path $ReportJson)) {
    if (-not $Verbose -and $godotOut) {
        Write-ReportText "--- Godot Engine Output (tail) ---" -ForegroundColor Red -IsError
        $godotOut | Select-Object -Last 30 | ForEach-Object { Write-ReportText $_ -ForegroundColor Red -IsError }
    }
    Show-StepFailure -Step "Step [4/5] Run" -Message "Test run produced no results file at $ReportJson (exit $RunCode); the suite likely aborted during startup." -Code $(if ($RunCode -ne 0) { $RunCode } else { 1 })
}
Stop-OrphanFixtureProcess -FixturePath $FixtureRoot
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
$doc = $null
try {
    $doc = Get-Content -LiteralPath $ReportJson -Raw | ConvertFrom-Json
    $t = $doc.totals
    if ($t) {
        $crashedNote = if ($t.crashed) { ", crashed: $($t.crashed)" } else { "" }
        $color = if ($ReportCode -eq 0) { "Green" } else { "Red" }
        Write-ReportText "Tests: passed $($t.pass), failed $($t.fail), skipped $($t.skip)$crashedNote" -ForegroundColor $color
    }
} catch {
    Write-ReportText "Tests: could not parse totals from $ReportJson" -ForegroundColor Yellow
}
if ($ReportCode -ne 0) {
    if (-not $Verbose -and $reportOut) {
        Write-ReportText "--- Full Test Report ---" -ForegroundColor DarkGray -IsError
        $reportOut | ForEach-Object { Write-ReportText $_ -ForegroundColor DarkGray -IsError }
    }
    if ($doc -and $doc.results) {
        $failedResults = @($doc.results | Where-Object { $_.status -in @("fail", "crashed") })
        if ($failedResults.Count -gt 0) {
            Write-ReportText "" -IsError
            Write-ReportText "================================================================================" -ForegroundColor Red -IsError
            Write-ReportText " Failed Test Details ($($failedResults.Count))" -ForegroundColor White -BackgroundColor DarkRed -IsError
            Write-ReportText "================================================================================" -ForegroundColor Red -IsError
            foreach ($res in $failedResults) {
                Write-ReportText " [FAIL] $($res.suite).$($res.name)" -ForegroundColor Red -IsError
                if ($res.failures -and $res.failures.Count -gt 0) {
                    foreach ($f in $res.failures) {
                        $loc = if ($f.file) { "$($f.file):$($f.line): " } else { "" }
                        $lines = if ($f.message) { @($f.message -split "`r?`n") } else { @("failed without a recorded message") }
                        Write-ReportText "     $loc$($lines[0])" -ForegroundColor Red -IsError
                        for ($i = 1; $i -lt $lines.Count; $i++) {
                            Write-ReportText "       $($lines[$i])" -ForegroundColor Yellow -IsError
                        }
                    }
                } else {
                    Write-ReportText "     failed or crashed without a recorded assertion failure" -ForegroundColor Red -IsError
                }
            }
            Write-ReportText "================================================================================" -ForegroundColor Red -IsError
        }
    }
    Show-StepFailure -Step "Step [5/5] Report" -Message "Tests failed or report could not be generated (see console report above)." -Code $ReportCode
}

Stop-OrphanFixtureProcess -FixturePath $FixtureRoot
Show-RunSuccess -Message "All test stages completed successfully!"
exit 0
