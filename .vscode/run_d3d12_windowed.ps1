<#
.SYNOPSIS
    Windowed D3D12 gdextest runner with external auto-close gating.

.DESCRIPTION
    DirectStorage D3D12 test runner helper. Drives Godot in
    --editor --windowed --rendering-driver d3d12 mode so a REAL D3D12
    RenderingDevice is created (headless uses RasterizerDummy -> null RD,
    which forces the DirectStorage Win32Backend to fall back).

    NOTE: The test harness requires --editor because gdextest is implemented
    as an EditorPlugin (extends EditorPlugin in addons/gdextest/plugin.gd)
    that only initializes after the editor's filesystem scan completes.
    Without --editor, there is no Plugin lifecycle to bootstrap the test run.

    Shutdown behavior (verified 2026-09-22): tree->quit() honors cleanly
    once tests complete (JSON written, GDExtension context shut down,
    logs close within ~1s). Both backup gates (--quit-after and the
    external watchdog Kill) are now COMMENTED OUT because they never fired.
    Re-enable them if a future run hangs.

    Output is redirected at the OS level (Start-Process -RedirectStandard*)
    so buffered logs survive a shutdown crash.

.PARAMETER Filter
    gdextest name-glob filter (e.g. "gpu_*"). Matched against suite.name.

.PARAMETER JsonName
    Result JSON filename, written inside the fixture project dir.

.PARAMETER QuitAfterFrames
    --quit-after value (backup auto-close). Default 600. NOTE: redundant
    since tree->quit() honors cleanly; the flag is commented out below.

.PARAMETER TimeoutSec
    External watchdog timeout in seconds. Default 120. NOTE: the kill gate
    is commented out below; kept as a tunable for manual re-enable.
#>
param(
    [string]$Filter = "gpu_*",
    [string]$JsonName = "d3d12_windowed.json",
    [int]$QuitAfterFrames = 600,
    [int]$TimeoutSec = 120,
    [string]$GodotExe = $env:GODOT,
    [switch]$Verbose
)

$ScriptStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

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

function Write-VerboseHost {
    param(
        [string]$Message,
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray
    )
    if ($Verbose) {
        Write-Host $Message -ForegroundColor $ForegroundColor
    }
}

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $RepoRoot

if (-not $GodotExe) {
    $GodotExe = Get-GodotConsolePath -RepoRoot $RepoRoot
}
if (-not $GodotExe) {
    Write-Error "No Godot binary: pass -GodotExe, set GODOT, or set GODOT_CONSOLE_PATH in .env."
    exit 1
}

$FixtureRoot = "build/gdextest/project"
$JsonRel = Join-Path $FixtureRoot $JsonName
$LogOut = Join-Path $PSScriptRoot "d3d12_stdout.log"
$LogErr = Join-Path $PSScriptRoot "d3d12_stderr.log"
# gdextest test library name (see [gdextest.output] name in .gdextest.toml);
# used to spot leftover build artifacts in bin/ that are not the loaded one.
$TestLibraryName = "libdsgd_tests"

# This runner does not regenerate the fixture, so it must use exactly the
# library the fixture manifest declares. Copying a differently-named binary
# writes a file Godot never loads and leaves the stale fixture copy in place;
# the run then reports "0 tests ran" while the freshly built DLL sits unused
# in bin/ (observed: a leftover *editor* target DLL was preferred by name).
$FixtureBin = Join-Path $FixtureRoot "addons/gdextest/bin"
if (-not (Test-Path -LiteralPath $FixtureBin)) {
    Write-Error "Fixture bin not found at $FixtureBin. Run .vscode/run_gdextest_local.ps1 once to generate the fixture."
    exit 1
}
$ManifestPath = Join-Path $FixtureRoot "addons/gdextest/gdextest.gdextension"
if (-not (Test-Path -LiteralPath $ManifestPath)) {
    Write-Host "ERROR: Fixture manifest not found at $ManifestPath. Run .vscode/run_gdextest_local.ps1 once to generate the fixture." -ForegroundColor Red
    exit 1
}
$DeclaredLibrary = $null
foreach ($manifestLine in (Get-Content -LiteralPath $ManifestPath)) {
    $libraryMatch = [regex]::Match($manifestLine, '^\s*windows\.[A-Za-z0-9_.]+\s*=\s*"res://addons/gdextest/bin/([^"]+)"')
    if ($libraryMatch.Success) {
        $DeclaredLibrary = $libraryMatch.Groups[1].Value
        break
    }
}
if (-not $DeclaredLibrary) {
    Write-Host "ERROR: No windows.* library entry in $ManifestPath. Regenerate the fixture with .vscode/run_gdextest_local.ps1." -ForegroundColor Red
    exit 1
}
$TestDllPath = Join-Path (Join-Path $RepoRoot "bin") $DeclaredLibrary
if (-not (Test-Path -LiteralPath $TestDllPath)) {
    Write-Host "ERROR: The fixture manifest declares '$DeclaredLibrary' but bin/$DeclaredLibrary is missing. Rebuild with .vscode/build_remote.ps1 -IncludeTests, or regenerate the fixture so the manifest matches the current build." -ForegroundColor Red
    exit 1
}
$TestDll = Get-Item -LiteralPath $TestDllPath

# Leftovers from another target/configuration are never cleaned by
# build_remote.ps1's target-scoped [5/6] cleanup. They are not loaded, but
# they are exactly how a mis-selection happens - surface them.
$OtherTestDlls = @(Get-ChildItem -Path (Join-Path $RepoRoot "bin") -Filter "$TestLibraryName*.dll" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $TestDll.Name })
if ($OtherTestDlls.Count -gt 0) {
    $others = ($OtherTestDlls | ForEach-Object { "{0} ({1:yyyy-MM-dd HH:mm})" -f $_.Name, $_.LastWriteTime }) -join ", "
    Write-Host "Warning: $($OtherTestDlls.Count) other test library/ies in bin/ (not loaded): $others" -ForegroundColor Yellow
}

# Freshness gate: a test library older than any input it must contain is stale,
# and running it silently tests the previous build.
$NewestInput = $null
foreach ($inputRoot in @("tests", "src", "extern/gdextest")) {
    $inputPath = Join-Path $RepoRoot $inputRoot
    if (-not (Test-Path -LiteralPath $inputPath)) {
        continue
    }
    $candidate = Get-ChildItem -Path $inputPath -Recurse -File -Include *.cpp, *.h, *.hpp -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\src\\gen\\' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($candidate -and (-not $NewestInput -or $candidate.LastWriteTime -gt $NewestInput.LastWriteTime)) {
        $NewestInput = $candidate
    }
}
$TomlPath = Join-Path $RepoRoot ".gdextest.toml"
if (Test-Path -LiteralPath $TomlPath) {
    $tomlItem = Get-Item -LiteralPath $TomlPath
    if (-not $NewestInput -or $tomlItem.LastWriteTime -gt $NewestInput.LastWriteTime) {
        $NewestInput = $tomlItem
    }
}
if ($NewestInput -and $NewestInput.LastWriteTime -gt $TestDll.LastWriteTime) {
    $newestRel = $NewestInput.FullName.Substring($RepoRoot.Length + 1)
    Write-Host ("ERROR: Stale test library: {0} ({1:yyyy-MM-dd HH:mm:ss}) is older than {2} ({3:yyyy-MM-dd HH:mm:ss}). Rebuild with .vscode/build_remote.ps1 -IncludeTests." -f $TestDll.Name, $TestDll.LastWriteTime, $newestRel, $NewestInput.LastWriteTime) -ForegroundColor Red
    exit 1
}
$FixtureDll = Join-Path $FixtureBin $TestDll.Name
Copy-Item -LiteralPath $TestDll.FullName -Destination $FixtureDll -Force
# The fixture copy is what Godot actually loads. Prove it is the file we chose:
# "selected" and "loaded" silently diverging is the whole failure mode.
$sourceHash = (Get-FileHash -LiteralPath $TestDll.FullName -Algorithm SHA256).Hash
$fixtureHash = (Get-FileHash -LiteralPath $FixtureDll -Algorithm SHA256).Hash
if ($sourceHash -ne $fixtureHash) {
    Write-Host "ERROR: Fixture copy of $($TestDll.Name) does not match bin/ after copy (SHA256 mismatch). Refusing to run a binary that is not the one selected." -ForegroundColor Red
    exit 1
}
Write-Host ("Test library: {0} ({1:yyyy-MM-dd HH:mm:ss}) -> {2}" -f $TestDll.Name, $TestDll.LastWriteTime, $FixtureDll) -ForegroundColor DarkGray

# Stage the DirectStorage runtime beside the fixture's addon copy: without
# dstorage*.dll the Win32Backend init fails and every GPU suite silently
# skips (observed all-skip on D3D12 hardware). Refresh every run; a missing
# local runtime warns once and keeps the old skip behavior.
$RuntimeSrcDir = Join-Path $RepoRoot "project/addons/directstorage_accelerator/bin"
$FixtureAddonBin = Join-Path $FixtureRoot "addons/directstorage_accelerator/bin"
$RuntimeDlls = @(Get-ChildItem -Path $RuntimeSrcDir -Filter "dstorage*.dll" -File -ErrorAction SilentlyContinue)
if ($RuntimeDlls.Count -eq 0) {
    Write-Host "Warning: no dstorage*.dll in $RuntimeSrcDir; D3D12 GPU suites will skip." -ForegroundColor Yellow
}
else {
    if (-not (Test-Path -LiteralPath $FixtureAddonBin)) {
        New-Item -ItemType Directory -Force -Path $FixtureAddonBin | Out-Null
    }
    foreach ($runtimeDll in $RuntimeDlls) {
        Copy-Item -LiteralPath $runtimeDll.FullName -Destination (Join-Path $FixtureAddonBin $runtimeDll.Name) -Force
    }
    Write-VerboseHost "Staged $($RuntimeDlls.Count) DirectStorage runtime DLL(s) into fixture." -ForegroundColor DarkGray
}

# Remove stale results so a crash cannot be masked by an old JSON.
foreach ($f in @($JsonRel, $LogOut, $LogErr)) {
    if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
}
Get-ChildItem -Path $FixtureBin -Filter "~*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

$ArgList = @(
    "--editor",
    "--windowed",
    "--resolution", "64x64",
    "--rendering-driver", "d3d12",
    "--audio-driver", "Dummy",
    "--quit-after", "$QuitAfterFrames",
    "--path", $FixtureRoot,
    "--",
    "--gdextest-run",
    "--gdextest-filter=$Filter",
    "--gdextest-json=$JsonName"
)

Write-VerboseHost "Godot: $GodotExe" -ForegroundColor DarkGray
Write-VerboseHost "Args : $($ArgList -join ' ')" -ForegroundColor DarkGray
Write-VerboseHost "Running windowed D3D12 test (no kill gate; relying on tree->quit())..." -ForegroundColor Cyan

$p = Start-Process -FilePath $GodotExe -ArgumentList $ArgList `
    -RedirectStandardOutput $LogOut -RedirectStandardError $LogErr `
    -PassThru -NoNewWindow

# Watchdog kill gate DISABLED (verified 2026-09-22: tree->quit() exits cleanly,
# JSON + shutdown log written, process gone within ~1s; the gate never fired).
# Re-enable if a future run hangs. Original gate:
#   $timedOut = $false
#   if (-not $p.WaitForExit($TimeoutSec * 1000)) {
#       $timedOut = $true
#       Write-Host "Watchdog timeout (${TimeoutSec}s) - killing process." -ForegroundColor Yellow
#       try { $p.Kill() } catch {}
#       $p.WaitForExit(5000) | Out-Null
#   }
$timedOut = $false
try {
    Wait-Process -InputObject $p -ErrorAction SilentlyContinue
} catch {}
$exitCode = if ($null -ne $p.ExitCode) { $p.ExitCode } else { 0 }

if ($Verbose) {
    Write-Host ""
    Write-Host "=== Exit code: $exitCode (timedOut=$timedOut) ===" -ForegroundColor Cyan
    Write-Host "=== STDOUT (filtered) ===" -ForegroundColor Cyan
    if (Test-Path $LogOut) {
        Get-Content $LogOut | Select-String -Pattern "DirectStorage|D3D12|RenderingDevice|PASSED|FAILED|SKIP|gpu_|GDX_TEST_OUTPUT|backend|leaked|RID" | Select-Object -First 60
    }
    Write-Host "=== JSON produced? ===" -ForegroundColor Cyan
    if (Test-Path $JsonRel) {
        $size = (Get-Item $JsonRel).Length
        Write-Host "YES: $JsonRel ($size bytes)" -ForegroundColor Green
        Get-Content $JsonRel -Raw
    } else {
        Write-Host "NO JSON at $JsonRel" -ForegroundColor Red
    }
}

$testPassed = $false
if (Test-Path $JsonRel) {
    try {
        $doc = Get-Content -LiteralPath $JsonRel -Raw | ConvertFrom-Json
        $t = $doc.totals
        if ($t) {
            $crashedNote = if ($t.crashed) { ", crashed: $($t.crashed)" } else { "" }
            $totalRan = [int]$t.pass + [int]$t.fail + [int]$t.skip
            if ($Filter -and $totalRan -eq 0) {
                # A filter that matches nothing is never a pass: it means the
                # loaded library lacks the suite (stale build) or the name is
                # wrong. Both need a human, not a green banner.
                Write-Host ("Tests: filter '{0}' matched 0 tests (stale test library or wrong suite name) - loaded {1} from {2:yyyy-MM-dd HH:mm:ss}" -f $Filter, $TestDll.Name, $TestDll.LastWriteTime) -ForegroundColor Red
                $testPassed = $false
            } else {
                $isSuccess = ($exitCode -eq 0 -and $t.fail -eq 0 -and (-not $t.crashed -or $t.crashed -eq 0))
                $color = if ($isSuccess) { "Green" } else { "Red" }
                Write-Host "Tests: passed $($t.pass), failed $($t.fail), skipped $($t.skip)$crashedNote (exit code $exitCode)" -ForegroundColor $color
                $testPassed = $isSuccess
            }
        }
    } catch {
        Write-Host "Tests: could not parse totals from $JsonRel" -ForegroundColor Yellow
    }
} else {
    Write-Host "Tests: no results file generated at $JsonRel (exit code $exitCode)" -ForegroundColor Red
}

if (-not $testPassed -and -not $Verbose) {
    if (Test-Path $LogErr) {
        $errContent = Get-Content $LogErr -ErrorAction SilentlyContinue
        if ($errContent) {
            Write-Host "--- Stderr ---" -ForegroundColor Red
            $errContent | Select-Object -Last 20 | ForEach-Object { Write-Host $_ }
        }
    }
    if (Test-Path $LogOut) {
        $failLines = Get-Content $LogOut -ErrorAction SilentlyContinue | Select-String -Pattern "\[FAILED\]|ERROR:|error:|CRASH|EXCEPTION|FATAL"
        if ($failLines) {
            Write-Host "--- Relevant stdout errors ---" -ForegroundColor Red
            $failLines | Select-Object -Last 20 | ForEach-Object { Write-Host $_ }
        }
    }
}

$elapsedSecs = $ScriptStopwatch.Elapsed.TotalSeconds.ToString("F1")
if ($testPassed) {
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Green
    Write-Host " [RUN SUCCESS] D3D12 windowed tests completed successfully! " -ForegroundColor White -BackgroundColor DarkGreen
    Write-Host " Duration: ${elapsedSecs}s" -ForegroundColor Green
    Write-Host "================================================================================" -ForegroundColor Green
    exit 0
} else {
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host " [RUN FAILED] D3D12 windowed tests failed " -ForegroundColor White -BackgroundColor DarkRed
    Write-Host " Duration: ${elapsedSecs}s" -ForegroundColor DarkGray
    Write-Host "================================================================================" -ForegroundColor Red
    exit $(if ($exitCode -ne 0) { $exitCode } else { 1 })
}
