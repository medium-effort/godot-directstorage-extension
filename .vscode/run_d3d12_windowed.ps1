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

    Shutdown behavior: the harness writes the result JSON and then quits the
    tree. The window disappears at the START of engine shutdown, while worker
    threads (DirectStorage completion thread, render-thread dispatch, D3D12
    teardown) are still being joined - so "window closed" and "process exited"
    are different events, and a thread that never joins keeps the process alive
    with no window and no output.

    That is why this runner bounds the run in two places instead of trusting
    tree->quit():

      1. --quit-after <frames> (--QuitAfterFrames). A FRAME count, not a
         deadline: in --editor mode frames only start after the filesystem
         scan, and an unfocused 64x64 window can run at a few FPS. It cannot
         bound start-up, and it cannot bound anything after the main loop ends
         because it is not evaluated once the loop is gone.
      2. A hard wall-clock bound (--TimeoutSec) on the WHOLE run, plus a
         shorter post-results grace (--ShutdownGraceSec). Results landing on
         disk end the "are the tests hung?" question and start the
         "is shutdown hung?" question; the two are reported separately because
         they mean different things.

    Both bounds are armed. A timeout fails the run and reports which phase it
    was in - it never reports success, because a killed run has no exit code
    and the JSON alone cannot prove the engine shut down.

    Output is redirected at the OS level (Start-Process -RedirectStandard*)
    so buffered logs survive a shutdown crash. Godot's stdout is block-buffered,
    so a killed run may lose its tail; the JSON is written and flushed by the
    harness and is the authoritative result.

    Concurrency: only one gdextest run may target this workspace at a time
    (tools/run_guard.ps1, workspace-keyed kernel mutex). A second run exits
    with code 2 before touching the fixture instead of dying later on a raw
    sharing violation.

.PARAMETER Filter
    gdextest name-glob filter (e.g. "gpu_*"). Matched against suite.name.

.PARAMETER JsonName
    Result JSON filename, written inside the fixture project dir.

.PARAMETER QuitAfterFrames
    --quit-after value (frame-count auto-close, passed through to the engine).
    Default 30: the editor's own exit path, kept as the graceful one. It is a
    frame count, so it cannot bound start-up or post-main-loop shutdown; the
    hard bound is -TimeoutSec.

.PARAMETER TimeoutSec
    Hard wall-clock bound on the whole run (launch -> process exit), in
    seconds. Default 180. On expiry the process tree is killed and the run
    FAILS. Must exceed the slowest expected suite; the windowed D3D12 suite
    can take minutes on a cold start even though the tests themselves are
    seconds.

.PARAMETER ShutdownGraceSec
    Once the result JSON appears, how long the engine gets to exit on its own
    before the run is reported as "results produced, shutdown did not
    complete". Default 15.
#>
param(
    [string]$Filter = "gpu_*",
    [string]$JsonName = "d3d12_windowed.json",
    [int]$QuitAfterFrames = 30,
    [int]$TimeoutSec = 180,
    [int]$ShutdownGraceSec = 15,
    [int]$MinFreeDiskMB = 256,
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

function Test-FixtureDiskSpace {
    param(
        [string]$Path,
        [int]$MinFreeMB = 256
    )
    if ($MinFreeMB -le 0) {
        return @{ Ok = $true; Drive = ""; FreeMB = 0; RequiredMB = $MinFreeMB }
    }
    try {
        $full = [System.IO.Path]::GetFullPath($Path)
        $root = [System.IO.Path]::GetPathRoot($full)
        if (-not $root) {
            return @{ Ok = $true; Drive = ""; FreeMB = 0; RequiredMB = $MinFreeMB }
        }
        $drive = [System.IO.DriveInfo]::new($root)
        if (-not $drive.IsReady) {
            return @{ Ok = $true; Drive = $root; FreeMB = 0; RequiredMB = $MinFreeMB }
        }
        $freeBytes = [int64]$drive.AvailableFreeSpace
        $freeMB = [int][Math]::Floor($freeBytes / 1MB)
        $requiredBytes = [int64]$MinFreeMB * 1MB
        return @{ Ok = ($freeBytes -ge $requiredBytes); Drive = $root; FreeMB = $freeMB; RequiredMB = $MinFreeMB }
    } catch {
        $null = $_
        return @{ Ok = $true; Drive = ""; FreeMB = 0; RequiredMB = $MinFreeMB }
    }
}

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $RepoRoot

# One run per workspace: see tools/run_guard.ps1 for the full list of
# collisions. Acquired before anything touches the fixture, because the -Force
# copy of the test library into the fixture bin throws a bare sharing violation
# when a live engine has it mapped, and that reads like a broken build.
. (Join-Path $RepoRoot "tools/run_guard.ps1")
$RunGuard = Enter-RunGuard -Name (Get-RunGuardName -Root $RepoRoot)
if ($RunGuard.Owned -eq $false) {
    Write-Host ("ERROR: another gdextest run is already active for this workspace (guard: {0}). A second run cannot produce a result: it would overwrite build/gdextest/project/addons/gdextest/bin while that engine has the library mapped, and it would clear this run's result JSON and redirect logs. Wait for it to finish - the OS releases the guard when that process exits, so it can never go stale." -f $RunGuard.FullName) -ForegroundColor Red
    exit 2
}
elseif ($null -eq $RunGuard.Owned) {
    Write-Host "[Guard] Warning: could not create the run guard; continuing without concurrency protection." -ForegroundColor Yellow
}
# $RunGuard (and the Mutex it references) must stay referenced for the rest of
# the run: dropping the last reference would let the GC release the mutex early.

$diskCheck = Test-FixtureDiskSpace -Path (Join-Path $RepoRoot "build/gdextest/project") -MinFreeMB $MinFreeDiskMB
if (-not $diskCheck.Ok) {
    Write-Host ("ERROR: Insufficient free disk space on {0}: {1} MiB available, need at least {2} MiB for fixture staging, Godot GDExtension shadow copy, and test logs. Free space and retry (or pass -MinFreeDiskMB 0 to bypass)." -f $diskCheck.Drive, $diskCheck.FreeMB, $diskCheck.RequiredMB) -ForegroundColor Red
    exit 1
}

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

# Remove stale results so a crash cannot be masked by an old JSON. A run that
# was killed leaves Godot holding the redirect files, and Start-Process then
# fails with an unhelpful "file in use" - verify the clear instead of assuming.
foreach ($f in @($JsonRel, $LogOut, $LogErr)) {
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
}
$stillLocked = @(@($JsonRel, $LogOut, $LogErr) | Where-Object { Test-Path -LiteralPath $_ })
if ($stillLocked.Count -gt 0) {
    Write-Host ("ERROR: could not clear stale run artifacts: {0}. A previous Godot run may still be alive; stop it and retry." -f ($stillLocked -join ", ")) -ForegroundColor Red
    exit 1
}
Get-ChildItem -Path $FixtureBin -Filter "~*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$lockedShadow = @(Get-ChildItem -Path $FixtureBin -Filter "~*.dll" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
if ($lockedShadow.Count -gt 0) {
    Write-Host ("ERROR: Locked shadow DLL(s) remain in fixture bin: {0}. Another Godot process still holds the test library mapped; stop it and retry." -f ($lockedShadow -join ", ")) -ForegroundColor Red
    exit 1
}

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
Write-VerboseHost "Running windowed D3D12 test (hard bound ${TimeoutSec}s, shutdown grace ${ShutdownGraceSec}s)..." -ForegroundColor Cyan

$p = Start-Process -FilePath $GodotExe -ArgumentList $ArgList `
    -RedirectStandardOutput $LogOut -RedirectStandardError $LogErr `
    -PassThru -NoNewWindow
$null = $p.Handle

# Bounded wait in two phases.
#
# Phase A (no results yet): the run is bounded by $TimeoutSec only. --quit-after
# is already on the command line, but it counts frames and is not evaluated
# before the main loop starts, so it cannot bound start-up.
#
# Phase B (results written): the JSON is on disk, so the tests are done. What
# remains is engine shutdown - and that is the phase that hangs, because the
# window is destroyed before the worker threads are joined. Give it
# $ShutdownGraceSec, then report it as its own failure instead of blaming the
# suite.
function Stop-ProcessTree {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [System.Diagnostics.Process]$Process,
        [int]$GraceMs = 5000
    )
    if ($null -eq $Process) {
        return
    }
    if (-not $PSCmdlet.ShouldProcess("PID $($Process.Id)", "Terminate process tree")) {
        return
    }
    try {
        $allProcs = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
        $toVisit = [System.Collections.Generic.Queue[int]]::new()
        $descendants = [System.Collections.Generic.List[int]]::new()
        $seen = [System.Collections.Generic.HashSet[int]]::new()
        $rootPid = [int]$Process.Id
        $null = $seen.Add($rootPid)
        $toVisit.Enqueue($rootPid)
        while ($toVisit.Count -gt 0) {
            $parentId = $toVisit.Dequeue()
            foreach ($entry in $allProcs) {
                if ([int]$entry.ParentProcessId -eq $parentId) {
                    $cid = [int]$entry.ProcessId
                    if ($seen.Add($cid)) {
                        $descendants.Add($cid)
                        $toVisit.Enqueue($cid)
                    }
                }
            }
        }
        for ($i = $descendants.Count - 1; $i -ge 0; $i--) {
            try {
                Stop-Process -Id $descendants[$i] -Force -ErrorAction SilentlyContinue
            } catch {
                $null = $_
            }
        }
    } catch {
        $null = $_
    }
    try {
        if (-not $Process.HasExited) {
            $Process.Kill()
        }
    } catch {
        $null = $_
    }
    try {
        $null = $Process.WaitForExit($GraceMs)
    } catch {
        $null = $_
    }
}

$timedOut = $false
$shutdownHung = $false
$resultsAtSec = $null
while (-not $p.HasExited) {
    $elapsedSec = $ScriptStopwatch.Elapsed.TotalSeconds
    if ($null -eq $resultsAtSec -and (Test-Path -LiteralPath $JsonRel)) {
        $resultsAtSec = $elapsedSec
        Write-VerboseHost ("Results written at {0:F1}s; waiting up to {1}s for engine shutdown." -f $resultsAtSec, $ShutdownGraceSec) -ForegroundColor DarkGray
    }
    if ($null -ne $resultsAtSec -and -not $shutdownHung -and ($elapsedSec - $resultsAtSec) -ge $ShutdownGraceSec) {
        $shutdownHung = $true
        Write-Host ("WARNING: results were written {0:F1}s ago but the engine is still running (window already closed). This is a shutdown-thread hang, not a test hang; waiting for the {1}s hard bound." -f ($elapsedSec - $resultsAtSec), $TimeoutSec) -ForegroundColor Yellow
    }
    if ($elapsedSec -ge $TimeoutSec) {
        $timedOut = $true
        break
    }
    Start-Sleep -Milliseconds 200
}

if ($timedOut) {
    $phase = if ($null -ne $resultsAtSec) { ("engine shutdown (results were written at {0:F1}s)" -f $resultsAtSec) } else { "test execution (no results written)" }
    Write-Host ("Watchdog timeout: hard bound of {0}s reached during {1} - killing process tree." -f $TimeoutSec, $phase) -ForegroundColor Yellow
    Stop-ProcessTree -Process $p -GraceMs 5000
} else {
    # Exited on its own; collect the code before anything else touches $p.
    $null = $p.WaitForExit(5000)
}

# Never manufacture an exit code: an unknown code must not read as success.
# A code read after our own Kill() describes the kill, not the engine, so it is
# deliberately not collected.
$exitCode = $null
if (-not $timedOut -and $p.HasExited) {
    try { $exitCode = [int]$p.ExitCode } catch { $exitCode = $null }
}
$exitAtSec = $ScriptStopwatch.Elapsed.TotalSeconds
$resultsAtText = if ($null -ne $resultsAtSec) { ("{0:F1}s" -f $resultsAtSec) } else { "never" }

if ($Verbose) {
    Write-Host ""
    $exitText = if ($null -eq $exitCode) { "no exit code: process was killed" } else { "exit code $exitCode" }
    Write-Host "=== Exit: $exitText (timedOut=$timedOut, resultsAt=$resultsAtText) ===" -ForegroundColor Cyan
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
$failureReason = $null
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
                $failureReason = "filter matched 0 tests"
                $testPassed = $false
            } else {
                $testsGreen = ($t.fail -eq 0 -and (-not $t.crashed -or $t.crashed -eq 0))
                $exitOk = ($null -ne $exitCode -and $exitCode -eq 0)
                $exitText = if ($null -eq $exitCode) { "no exit code: process was killed" } else { "exit code $exitCode" }
                $color = if ($testsGreen -and $exitOk -and -not $timedOut) { "Green" } else { "Red" }
                Write-Host "Tests: passed $($t.pass), failed $($t.fail), skipped $($t.skip)$crashedNote ($exitText)" -ForegroundColor $color
                if ($timedOut) {
                    # Results can be complete while the engine never exits; say
                    # which one happened instead of implying the suite failed.
                    Write-Host ("Shutdown: NOT CLEAN - hard bound {0}s reached and the process was killed." -f $TimeoutSec) -ForegroundColor Red
                    $failureReason = "engine did not exit (killed at ${TimeoutSec}s)"
                } elseif ($null -eq $exitCode) {
                    Write-Host "Shutdown: NOT CLEAN - process exited without a readable exit code." -ForegroundColor Red
                    $failureReason = "engine exit code unreadable"
                } elseif ($exitCode -ne 0) {
                    Write-Host ("Shutdown: engine exited with code {0}." -f $exitCode) -ForegroundColor Red
                    $failureReason = "engine exit code $exitCode"
                } elseif ($testsGreen) {
                    Write-Host ("Shutdown: clean (engine exited {0:F1}s after launch)." -f $exitAtSec) -ForegroundColor DarkGray
                }
                $testPassed = ($testsGreen -and $exitOk -and -not $timedOut)
            }
        } else {
            Write-Host "Tests: results JSON at $JsonRel has no 'totals' block" -ForegroundColor Red
            $failureReason = "results JSON missing totals"
        }
    } catch {
        Write-Host "Tests: could not parse totals from $JsonRel" -ForegroundColor Yellow
        $failureReason = "unparseable results JSON"
    }
} else {
    Write-Host "Tests: no results file generated at $JsonRel" -ForegroundColor Red
    $failureReason = if ($timedOut) { "no results within ${TimeoutSec}s (killed)" } else { "no results file" }
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
    Write-Host " Duration: ${elapsedSecs}s (results at $resultsAtText, clean exit at $('{0:F1}s' -f $exitAtSec))" -ForegroundColor Green
    Write-Host "================================================================================" -ForegroundColor Green
    exit 0
} else {
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host " [RUN FAILED] D3D12 windowed run failed: $failureReason " -ForegroundColor White -BackgroundColor DarkRed
    Write-Host " Duration: ${elapsedSecs}s (results at $resultsAtText)" -ForegroundColor DarkGray
    Write-Host "================================================================================" -ForegroundColor Red
    exit $(if ($null -ne $exitCode -and $exitCode -ne 0) { $exitCode } else { 1 })
}
