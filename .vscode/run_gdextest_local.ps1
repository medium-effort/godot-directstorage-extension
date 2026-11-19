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
      [2/5] fixture - `generate_fixture.py` with the newest test library
                      in bin/ ($TestLibraryName), then manual staging of
                      `project/test_assets/test.png` per [gdextest.fixture] assets.
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
    [int]$WarmupTimeoutSec = 60,
    [int]$RunTimeoutSec = 180,
    [int]$MinFreeDiskMB = 256,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"

$ScriptStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Get-FormattedElapsed {
    param([System.Diagnostics.Stopwatch]$Stopwatch)
    if (-not $Stopwatch) { return "" }
    $elapsed = $Stopwatch.Elapsed
    if ($elapsed.TotalMinutes -ge 1) {
        $mins = [int][Math]::Floor($elapsed.TotalMinutes)
        return ("{0}m {1:D2}s ({2:F1}s total)" -f $mins, $elapsed.Seconds, $elapsed.TotalSeconds)
    }
    return ("{0:F1}s" -f $elapsed.TotalSeconds)
}

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
    $elapsedStr = Get-FormattedElapsed $ScriptStopwatch
    Write-ReportText "" -IsError
    Write-ReportText "================================================================================" -ForegroundColor Red -IsError
    Write-ReportText " [RUN FAILED] $Step " -ForegroundColor White -BackgroundColor DarkRed -IsError
    Write-ReportText "================================================================================" -ForegroundColor Red -IsError
    Write-ReportText " Reason: $Message" -ForegroundColor Red -IsError
    if ($Code -ne 0) {
        Write-ReportText " Exit Code: $Code" -ForegroundColor Yellow -IsError
    }
    if ($elapsedStr) {
        Write-ReportText " Elapsed Time: $elapsedStr" -ForegroundColor DarkGray -IsError
    }
    Write-ReportText " Check the terminal output above for details." -ForegroundColor DarkGray -IsError
    Write-ReportText "================================================================================" -ForegroundColor Red -IsError
    Write-ReportText "" -IsError
    exit $Code
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

function Invoke-GodotBounded {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [int]$TimeoutSec,
        [string]$StepName,
        [string]$LogDir,
        [string]$LogPrefix
    )
    if (-not (Test-Path -LiteralPath $LogDir)) {
        $null = New-Item -ItemType Directory -Force -Path $LogDir
    }
    $outLog = Join-Path $LogDir ("{0}_stdout.log" -f $LogPrefix)
    $errLog = Join-Path $LogDir ("{0}_stderr.log" -f $LogPrefix)
    foreach ($logPath in @($outLog, $errLog)) {
        if (Test-Path -LiteralPath $logPath) {
            Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $logPath) {
            return @{ Code = 1; TimedOut = $false; Output = @(); OutLog = $outLog; ErrLog = $errLog; LockedLog = $logPath }
        }
    }

    $quotedArgs = @()
    foreach ($a in $Arguments) {
        if ($null -eq $a -or $a -eq "") {
            $quotedArgs += '""'
        } elseif ($a -match '[\s"]') {
            $quotedArgs += ('"' + $a.Replace('"', '\"') + '"')
        } else {
            $quotedArgs += $a
        }
    }

    # Q59: always launch Godot *console*.exe with file-based redirection, never
    # ProcessStartInfo stdout/stderr pipes, or the console shim hangs indefinitely.
    $proc = Start-Process -FilePath $Exe -ArgumentList $quotedArgs `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
        -PassThru -NoNewWindow
    $null = $proc.Handle

    $timedOut = $false
    $exitCode = 1
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        while (-not $proc.HasExited) {
            if ($TimeoutSec -gt 0 -and $sw.Elapsed.TotalSeconds -ge $TimeoutSec) {
                $timedOut = $true
                Write-ReportText ("Godot exceeded the {0}s {1} timeout; terminating process tree so it cannot keep the fixture DLL mapped." -f $TimeoutSec, $StepName) -ForegroundColor Yellow -IsError
                Stop-ProcessTree -Process $proc -GraceMs 5000
                break
            }
            Start-Sleep -Milliseconds 200
        }
        if (-not $timedOut) {
            $null = $proc.WaitForExit(5000)
        }
        if ($timedOut) {
            $exitCode = 124
        } elseif ($proc.HasExited) {
            try {
                if ($null -ne $proc.ExitCode) {
                    $exitCode = [int]$proc.ExitCode
                }
            } catch {
                $null = $_
                $exitCode = 1
            }
        }
    } finally {
        try {
            if (-not $proc.HasExited) {
                Stop-ProcessTree -Process $proc -GraceMs 5000
            }
        } catch {
            $null = $_
        }
        $proc.Dispose()
    }

    $outputLines = @()
    foreach ($logPath in @($outLog, $errLog)) {
        if (Test-Path -LiteralPath $logPath) {
            $lines = Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue
            if ($lines) {
                $outputLines += @($lines)
            }
        }
    }
    return @{ Code = $exitCode; TimedOut = $timedOut; Output = $outputLines; OutLog = $outLog; ErrLog = $errLog; LockedLog = $null }
}

function Stop-OrphanFixtureProcess {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [string]$FixturePath
    )
    $resolved = ""
    if ($FixturePath) {
        $resolved = [System.IO.Path]::GetFullPath($FixturePath)
    }
    $resolvedFwd = $resolved.Replace('\', '/')
    if (-not $PSCmdlet.ShouldProcess($resolved, "Stop orphan Godot fixture processes and clean shadow DLLs")) {
        return @()
    }
    try {
        $godotProcs = Get-Process -Name "*godot*" -ErrorAction SilentlyContinue
        if ($godotProcs) {
            # Match on this worktree's ABSOLUTE fixture path only. The engine is
            # always launched with an absolute --path, so the path in the command
            # line identifies the worktree unambiguously. A repo-wide pattern
            # ("build\gdextest\project" or "--gdextest-run") also matches a
            # parallel agent's fixture process in another worktree, and killing
            # that one is a cross-agent failure that looks like a random test
            # crash. Trade-off: an orphan left by an older runner that passed a
            # relative --path is not reaped here; the locked-DLL check below
            # still reports it instead of silently proceeding.
            foreach ($p in $godotProcs) {
                $match = $false
                try {
                    $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)" -ErrorAction SilentlyContinue
                    if ($resolved -and $cim -and $cim.CommandLine) {
                        $cmd = $cim.CommandLine
                        if ($cmd.IndexOf($resolved, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                            $cmd.IndexOf($resolvedFwd, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                            $match = $true
                        }
                    }
                } catch {
                    $null = $_
                }
                if ($match) {
                    Write-ReportText "Terminating orphan Godot fixture process (PID $($p.Id))..." -ForegroundColor Yellow
                    try {
                        $children = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($p.Id)" -ErrorAction SilentlyContinue
                        foreach ($child in $children) {
                            Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue
                        }
                    } catch {
                        $null = $_
                    }
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
                # Stale DLL is still locked; re-check only this worktree's fixture processes before retrying
                $remainingProcs = Get-Process -Name "*godot*" -ErrorAction SilentlyContinue
                foreach ($p in $remainingProcs) {
                    try {
                        $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)" -ErrorAction SilentlyContinue
                        if ($resolved -and $cim -and $cim.CommandLine) {
                            $cmd = $cim.CommandLine
                            if ($cmd.IndexOf($resolved, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                                $cmd.IndexOf($resolvedFwd, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
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
        $remainingLocked = @(Get-ChildItem -Path $binDir -Filter "~*.dll" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        return $remainingLocked
    }
    return @()
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
    $elapsedStr = Get-FormattedElapsed $ScriptStopwatch
    Write-ReportText ""
    Write-ReportText "================================================================================" -ForegroundColor Green
    Write-ReportText " [RUN SUCCESS] $Message " -ForegroundColor White -BackgroundColor DarkGreen
    if ($elapsedStr) {
        Write-ReportText " Duration: $elapsedStr" -ForegroundColor Green
    }
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
# Absolute form of the same path, anchored to the repo root rather than the
# process cwd. Passed to Godot as --path so the process command line carries a
# path that is unique to THIS worktree: Stop-OrphanFixtureProcess can then match
# on it instead of a repo-wide pattern that would also match a parallel agent's
# fixture process in another worktree.
$FixtureAbs = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSScriptRoot) $FixtureRoot))

# One run per workspace: see tools/run_guard.ps1. Acquired before the [2/5]
# fixture stage, because that stage regenerates $FixtureRoot and its
# Stop-OrphanFixtureProcess taskkills leftover Godot processes started against
# it - so starting now would kill a windowed run mid-suite instead of waiting
# for it.
. (Join-Path $RepoRoot "tools/run_guard.ps1")
$RunGuard = Enter-RunGuard -Name (Get-RunGuardName -Root $RepoRoot)
if ($RunGuard.Owned -eq $false) {
    Show-StepFailure -Step "Run Concurrency Guard" -Message ("Another gdextest run is already active for this workspace (guard: {0}). This runner regenerates {1} and taskkills leftover Godot processes started against it, so starting now would kill that run mid-suite. Wait for it to finish - the OS releases the guard when that process exits, so it can never go stale." -f $RunGuard.FullName, $FixtureRoot) -Code 2
}
elseif ($null -eq $RunGuard.Owned) {
    Write-ReportText "[Guard] Warning: could not create the run guard; continuing without concurrency protection." -ForegroundColor Yellow
}
# $RunGuard (and the Mutex it references) must stay referenced for the rest of
# the run: dropping the last reference would let the GC release the mutex early.

$diskCheck = Test-FixtureDiskSpace -Path $FixtureAbs -MinFreeMB $MinFreeDiskMB
if (-not $diskCheck.Ok) {
    Show-StepFailure -Step "Disk Space Preflight" -Message ("Insufficient free disk space on {0}: {1} MiB available, need at least {2} MiB for fixture staging, Godot GDExtension shadow copy, and test logs. Free space and retry (or pass -MinFreeDiskMB 0 to bypass)." -f $diskCheck.Drive, $diskCheck.FreeMB, $diskCheck.RequiredMB) -Code 1
}

# Artifact names mirror .gdextest.toml: [gdextest.output] name and
# [gdextest.fixture] project_name. Keep both in sync with that file.
$TestLibraryName = "libdsgd_tests"
$FixtureProjectName = "dsgd-tests"

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
$lockedShadow = @(Stop-OrphanFixtureProcess -FixturePath $FixtureAbs)
if ($lockedShadow.Count -gt 0) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message ("Locked shadow DLL(s) remain in fixture bin after reaping orphans: {0}. Another process still holds the test library mapped." -f ($lockedShadow -join ", ")) -Code 1
}
Write-VerboseHost "[2/5] fixture: generating Godot fixture project..." -ForegroundColor Cyan
$TestDll = Get-ChildItem -Path "./bin/$TestLibraryName*.dll" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $TestDll) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "No test library found at bin/$TestLibraryName*.dll. Build with .vscode/build_remote.ps1 -IncludeTests first." -Code 1
}
# Freshness gate: a test library older than its own inputs cannot contain them,
# so the run would silently test the previous build.
$NewestTestInput = $null
foreach ($inputRoot in @("tests", "src", "extern/gdextest")) {
    $inputPath = Join-Path $RepoRoot $inputRoot
    if (-not (Test-Path -LiteralPath $inputPath)) {
        continue
    }
    $candidate = Get-ChildItem -Path $inputPath -Recurse -File -Include *.cpp, *.h, *.hpp -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\src\\gen\\' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($candidate -and (-not $NewestTestInput -or $candidate.LastWriteTime -gt $NewestTestInput.LastWriteTime)) {
        $NewestTestInput = $candidate
    }
}
$TomlPath = Join-Path $RepoRoot ".gdextest.toml"
if (Test-Path -LiteralPath $TomlPath) {
    $tomlItem = Get-Item -LiteralPath $TomlPath
    if (-not $NewestTestInput -or $tomlItem.LastWriteTime -gt $NewestTestInput.LastWriteTime) {
        $NewestTestInput = $tomlItem
    }
}
if ($NewestTestInput -and $NewestTestInput.LastWriteTime -gt $TestDll.LastWriteTime) {
    $newestRel = $NewestTestInput.FullName.Substring($RepoRoot.Length + 1)
    Show-StepFailure -Step "Step [2/5] Fixture" -Message ("Stale test library: {0} ({1:yyyy-MM-dd HH:mm:ss}) is older than {2} ({3:yyyy-MM-dd HH:mm:ss}). Rebuild with .vscode/build_remote.ps1 -IncludeTests." -f $TestDll.Name, $TestDll.LastWriteTime, $newestRel, $NewestTestInput.LastWriteTime) -Code 1
}
Write-VerboseHost "Using test library: $($TestDll.FullName)" -ForegroundColor DarkGray
if ($Verbose) {
    & python extern/gdextest/tools/generate_fixture.py --project-root $FixtureRoot --library $TestDll.FullName --godot-version 4.7 --library-key windows.debug.x86_64 --project-name "$FixtureProjectName" --scan-timeout-ms 30000
} else {
    $fixtureOut = & python extern/gdextest/tools/generate_fixture.py --project-root $FixtureRoot --library $TestDll.FullName --godot-version 4.7 --library-key windows.debug.x86_64 --project-name "$FixtureProjectName" --scan-timeout-ms 30000 2>&1
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
# $AssetPatterns must stay identical to [gdextest.fixture] assets in
# .gdextest.toml: only the test.png fixture and its import stub. The 47 MB
# steampunk .glb is an interactive benchmark model no suite loads, and the
# steampunk_underwater_explorer_{0..5}.png.import stubs are orphans (their PNG
# sources are gitignored and absent). test.png is mandatory --
# tests/resource_loader_tests.cpp checks res://project/test_assets/test.png
# and GDEX_SKIPs when it is missing.
$AssetPatterns = @("test.png", "test.png.import")
Write-VerboseHost "Staging [gdextest.fixture] assets (project/test_assets/test.png)..." -ForegroundColor Cyan
$AssetSource = Join-Path $RepoRoot "project/test_assets"
if (-not (Test-Path $AssetSource)) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "Asset source project/test_assets/ not found; cannot stage fixture assets." -Code 1
}
$AssetFiles = @(foreach ($assetPattern in $AssetPatterns) {
    Get-ChildItem -Path $AssetSource -Filter $assetPattern -File
})
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
if (-not (Test-Path -LiteralPath (Join-Path $FixtureAssets "test.png"))) {
    Show-StepFailure -Step "Step [2/5] Fixture" -Message "Fixture asset test.png did not land in $FixtureRoot/project/test_assets/ (required by resource_loader_tests.cpp; suites would otherwise GDEX_SKIP silently)." -Code 1
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
$WarmupArgs += $FixtureAbs
$WarmupArgs += "--quit-after"
$WarmupArgs += "2"

$LogDir = Join-Path $RepoRoot "build/gdextest"
$WarmupCode = 1
$WarmupTimedOut = $false
for ($attempt = 1; $attempt -le 2; $attempt++) {
    $warmup = Invoke-GodotBounded -Exe $GodotExe -Arguments $WarmupArgs -TimeoutSec $WarmupTimeoutSec -StepName "warmup" -LogDir $LogDir -LogPrefix "local_warmup"
    if ($warmup.LockedLog) {
        Show-StepFailure -Step "Step [3/5] Warmup" -Message ("Warmup log file is locked by another process: {0}" -f $warmup.LockedLog) -Code 1
    }
    $WarmupCode = $warmup.Code
    $WarmupTimedOut = $warmup.TimedOut
    if ($WarmupCode -eq 0 -or $WarmupCode -eq 134) { break }
    if ($WarmupTimedOut) {
        $null = Stop-OrphanFixtureProcess -FixturePath $FixtureAbs
    }
    Write-VerboseHost "Warmup attempt $attempt exited $WarmupCode; retrying once..." -ForegroundColor Yellow
}
if ($WarmupCode -eq 134) {
    Write-VerboseHost "Warmup exited 134 (cold-cache shutdown abort; import completed -- expected per upstream docs/cli.md)." -ForegroundColor Yellow
} elseif ($WarmupTimedOut) {
    Show-StepFailure -Step "Step [3/5] Warmup" -Message ("Warmup exceeded {0}s timeout twice and process tree was terminated." -f $WarmupTimeoutSec) -Code 124
} elseif ($WarmupCode -ne 0) {
    Show-StepFailure -Step "Step [3/5] Warmup" -Message "Warmup pass failed twice." -Code $WarmupCode
}
$lockedAfterWarmup = @(Stop-OrphanFixtureProcess -FixturePath $FixtureAbs)
if ($lockedAfterWarmup.Count -gt 0) {
    Show-StepFailure -Step "Step [3/5] Warmup" -Message ("Locked shadow DLL(s) remain after warmup: {0}. Another process still holds the test library mapped." -f ($lockedAfterWarmup -join ", ")) -Code 1
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
$GodotArgs += $FixtureAbs
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
$runResult = Invoke-GodotBounded -Exe $GodotExe -Arguments $GodotArgs -TimeoutSec $RunTimeoutSec -StepName "test run" -LogDir $LogDir -LogPrefix "local_run"
if ($runResult.LockedLog) {
    Show-StepFailure -Step "Step [4/5] Run" -Message ("Test run log file is locked by another process: {0}" -f $runResult.LockedLog) -Code 1
}
$RunCode = $runResult.Code
$godotOut = $runResult.Output
if ($Verbose -and $godotOut) {
    foreach ($line in $godotOut) {
        Write-ReportText $line
    }
}
if ($runResult.TimedOut) {
    $null = Stop-OrphanFixtureProcess -FixturePath $FixtureAbs
    if (-not $Verbose -and $godotOut) {
        Write-ReportText "--- Godot Engine Output (tail) ---" -ForegroundColor Red -IsError
        $godotOut | Select-Object -Last 30 | ForEach-Object { Write-ReportText $_ -ForegroundColor Red -IsError }
    }
    $jsonNote = if (Test-Path $ReportJson) { "results JSON was written; engine shutdown likely hung" } else { "no results JSON written" }
    Show-StepFailure -Step "Step [4/5] Run" -Message ("Godot test run exceeded {0}s timeout and process tree was terminated ({1})." -f $RunTimeoutSec, $jsonNote) -Code 124
}
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
$null = Stop-OrphanFixtureProcess -FixturePath $FixtureAbs
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

Stop-OrphanFixtureProcess -FixturePath $FixtureAbs
Show-RunSuccess -Message "All test stages completed successfully!"
exit 0
