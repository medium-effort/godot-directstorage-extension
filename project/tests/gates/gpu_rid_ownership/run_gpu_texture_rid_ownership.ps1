<#
.SYNOPSIS
    GPU texture RID ownership suite (windowed D3D12).

.DESCRIPTION
    Runs project/tests/gates/gpu_rid_ownership/test_gpu_texture_rid_ownership.gd once per
    scenario in its own Godot process, then decides pass/fail from the
    GPU_RID_* markers plus the engine's own "RIDs of type Texture were leaked"
    exit census.

    This cannot live in gdextest: that suite runs headless, where Godot has no
    RenderingDevice, so DSResourceFormatLoader's GPU branch is unreachable.
    A real window is mandatory, so each run is small and parked in the
    bottom-right corner of the work area.

    Coverage: uncached/cache-disabled loads must not orphan their device RID,
    a RID released while its request is in flight must be deferred and then
    freed by the frame-drawn retry, prewarm must refuse up front when nothing
    can own its textures, and dropping a texture right before quit must leave a
    clean exit census.

.PARAMETER Expect
    fixed  (default) the invariant must hold - the suite must pass.
    bug    the suite must FAIL and show genuinely orphaned RIDs. Use it to
           prove the check is not vacuous (e.g. on a build with the old
           ownership bug); it is expected to fail on a healthy build.

.PARAMETER TimeoutSec
    Per-scenario OS-level timeout. Hangs cannot be detected in-process because
    the script's _initialize() is synchronous.

.PARAMETER GodotRoot
    Optional directory that contains the Godot console executable. When it is
    omitted, discovery uses $env:GODOT_ROOT, then the Godot directory listed in
    .agents/PATH_ALLOWLIST.md, then PATH.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File project/tests/gates/gpu_rid_ownership/run_gpu_texture_rid_ownership.ps1
    powershell -ExecutionPolicy Bypass -File project/tests/gates/gpu_rid_ownership/run_gpu_texture_rid_ownership.ps1 -Expect bug
#>
[CmdletBinding()]
param(
    # 'fixed' = the invariant must hold (default).
    # 'bug'   = the suite must be red AND show real orphaned RIDs, which proves
    #           the check is not vacuous. Expected to fail on a healthy build.
    [ValidateSet('fixed', 'bug')]
    [string]$Expect = 'fixed',

    # Optional scenario filter (comma-separated). Needed because -Expect applies
    # to the whole run: on a build that is already green for the probe scenarios,
    # a full-suite bug-mode run fails them and muddies the red evidence.
    [string[]]$Scenarios = @(),

    [ValidateRange(30, 1800)]
    [int]$TimeoutSec = 300,

    # Each scenario needs a real window (D3D12 has no headless RenderingDevice),
    # so keep it tiny and parked in the bottom-right corner instead of letting
    # Godot centre it over whatever the user is doing.
    [ValidateRange(64, 1280)]
    [int]$WindowWidth = 192,
    [ValidateRange(64, 720)]
    [int]$WindowHeight = 108,
    [ValidateRange(0, 256)]
    [int]$WindowMargin = 8,

    # Optional Godot versions directory. When it is omitted, the script uses
    # $env:GODOT_ROOT, then the Godot directory listed in
    # .agents/PATH_ALLOWLIST.md, then PATH.
    [string]$GodotRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptRoot = $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptRoot)
$ProjectPath = Join-Path $RepoRoot 'project'
$GdScript = Join-Path $ScriptRoot 'test_gpu_texture_rid_ownership.gd'
$LogDir = Join-Path $RepoRoot '.cache/gpu_texture_rid_ownership'

# Resolves the Godot versions directory without a machine-specific default: an
# explicit -GodotRoot wins, then $env:GODOT_ROOT, then the Godot entry in the
# workspace allowlist, then PATH. The directory must contain a console
# executable, because this suite needs a streamed stdout/stderr log.
if ([string]::IsNullOrWhiteSpace($GodotRoot)) {
    $Candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:GODOT_ROOT)) {
        $Candidates += $env:GODOT_ROOT
    }

    $AllowlistPath = Join-Path $RepoRoot '.agents/PATH_ALLOWLIST.md'
    if (Test-Path -LiteralPath $AllowlistPath) {
        foreach ($Line in Get-Content -LiteralPath $AllowlistPath) {
            $Entry = $Line.Trim()
            if ($Entry -eq '' -or $Entry.StartsWith('#')) {
                continue
            }
            # The file also carries HTML comment lines and bare command names;
            # only absolute Windows paths are directory candidates. Testing them
            # with IsPathRooted would throw on the comment characters.
            if ($Entry -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') {
                continue
            }
            $Candidates += $Entry
        }
    }

    foreach ($Name in @('godot*console*.exe', 'godot*.exe')) {
        $Command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $Command) {
            $Candidates += [string]$Command.Source
        }
    }

    foreach ($Candidate in $Candidates) {
        $Directory = $null
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            $Directory = Split-Path -Parent $Candidate
        } elseif (Test-Path -LiteralPath $Candidate -PathType Container) {
            $Directory = $Candidate
        }
        if ($null -eq $Directory) {
            continue
        }
        $Console = Get-ChildItem -Path $Directory -Filter '*console*.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $Console) {
            $GodotRoot = $Directory
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($GodotRoot)) {
    throw "Godot console executable not found. Pass -GodotRoot <directory>, set GODOT_ROOT, or add the Godot directory to .agents/PATH_ALLOWLIST.md."
}

if (-not (Test-Path -LiteralPath $ProjectPath)) {
    throw "Project directory not found: $ProjectPath"
}
if (-not (Test-Path -LiteralPath $GdScript)) {
    throw "Harness script not found: $GdScript"
}
if (-not (Test-Path -LiteralPath $GodotRoot)) {
    throw "Godot versions directory not found: $GodotRoot"
}
if (-not (Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

$ConsoleExe = Get-ChildItem -Path $GodotRoot -Recurse -Filter '*console*.exe' -File | Select-Object -First 1
if ($null -eq $ConsoleExe) {
    throw "Godot console executable not found under $GodotRoot"
}

# Parses the last marker line with the given prefix (empty string if absent).
function Get-MarkerLine {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)][string]$Prefix
    )
    $found = @($Text -split "`r?`n" | Where-Object { $_.StartsWith($Prefix) })
    $line = ''
    if ($found.Count -gt 0) {
        $line = [string]$found[$found.Count - 1]
    }
    return $line
}

# Returns the integer value of key=<int> inside a marker line, or -1.
function Get-MarkerInt {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Line,
        [Parameter(Mandatory = $true)][string]$Field
    )
    if ([string]::IsNullOrEmpty($Line)) {
        return -1
    }
    $match = [regex]::Match($Line, [regex]::Escape($Field) + '=(\d+)')
    if (-not $match.Success) {
        return -1
    }
    return [int]$match.Groups[1].Value
}

# Sums every "N RIDs of type "Texture" were leaked" warning in the captured
# output. This is the engine's own exit census, independent of the in-process
# oracle, so the two together cannot both lie the same way.
function Get-TextureLeakCensus {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $total = 0
    foreach ($match in [regex]::Matches($Text, '(\d+) RIDs? of type "Texture"')) {
        $total += [int]$match.Groups[1].Value
    }
    return $total
}

$AllScenarios = @('oracle', 'reuse', 'ignore', 'sim_fix', 'disabled_cache', 'prewarm_disabled_cache', 'teardown_immediate', 'teardown_prewarm_pending', 'census_control', 'deferral_retry', 'hammer')
# Scenarios whose outcome depends on the RID-ownership invariant holding.
$ProbeScenarios = @('ignore', 'disabled_cache', 'hammer')

# -Scenarios narrows the run; an unknown name is an error, not an ignore, so a
# typo cannot silently produce a green run that tested nothing.
$SelectedScenarios = $AllScenarios
if ($Scenarios.Count -gt 0) {
    $UnknownScenarios = @($Scenarios | Where-Object { $AllScenarios -notcontains $_ })
    if ($UnknownScenarios.Count -gt 0) {
        throw "Unknown scenario(s): $($UnknownScenarios -join ', '). Known: $($AllScenarios -join ', ')"
    }
    $SelectedScenarios = @($Scenarios)
}

# Window placement: a real window is mandatory (D3D12 has no headless
# RenderingDevice), so make it small and keep it out of the way in the
# bottom-right of the primary work area (above the taskbar) rather than letting
# Godot centre it over the user's work.
$ExtraWindowArgs = ''
$WindowDescription = 'centred (placement unavailable)'
$WorkArea = $null
try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    $WorkArea = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
} catch {
    Write-Output "Window placement unavailable: $($_.Exception.Message)"
}
if ($null -ne $WorkArea) {
    $WindowPosX = [Math]::Max(0, $WorkArea.Right - $WindowWidth - $WindowMargin)
    $WindowPosY = [Math]::Max(0, $WorkArea.Bottom - $WindowHeight - $WindowMargin)
    $ExtraWindowArgs = ' --resolution {0}x{1} --position {2},{3}' -f $WindowWidth, $WindowHeight, $WindowPosX, $WindowPosY
    $WindowDescription = "{0}x{1} at $WindowPosX,$WindowPosY (bottom-right, work area right=$($WorkArea.Right) bottom=$($WorkArea.Bottom))" -f $WindowWidth, $WindowHeight
}

Write-Output "Expectation : $Expect"
Write-Output "Scenarios   : $($SelectedScenarios -join ', ')"
Write-Output "Console     : $($ConsoleExe.FullName)"
Write-Output "Project     : $ProjectPath"
Write-Output "Harness     : $GdScript"
Write-Output "Window      : $WindowDescription"
Write-Output ''

$Rows = @()
$AnySkip = $false
$AnyFail = $false

foreach ($Scenario in $SelectedScenarios) {
    $StdOutPath = Join-Path $LogDir ($Scenario + '.stdout.log')
    $StdErrPath = Join-Path $LogDir ($Scenario + '.stderr.log')

    # Fixed quoting pattern shared with the other .vscode runners.
    # $ExtraWindowArgs carries the bottom-right --resolution/--position pair.
    $Arguments = '--path "{0}" --rendering-driver d3d12{3} -s "{1}" -- --scenario={2}' -f $ProjectPath, $GdScript, $Scenario, $ExtraWindowArgs

    $Process = Start-Process -FilePath $ConsoleExe.FullName -ArgumentList $Arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput $StdOutPath -RedirectStandardError $StdErrPath

    $TimedOut = -not $Process.WaitForExit($TimeoutSec * 1000)
    if ($TimedOut) {
        try {
            $Process.Kill()
        } catch {
            Write-Output "Kill failed for scenario $Scenario`: $($_.Exception.Message)"
        }
        $Process.WaitForExit(5000)
    }
    $ExitCode = $Process.ExitCode

    $Text = ''
    if (Test-Path -LiteralPath $StdOutPath) {
        $StdOutText = Get-Content -LiteralPath $StdOutPath -Raw
        if ($null -ne $StdOutText) {
            $Text = $StdOutText
        }
    }
    if (Test-Path -LiteralPath $StdErrPath) {
        $StdErrText = Get-Content -LiteralPath $StdErrPath -Raw
        if ($null -ne $StdErrText) {
            $Text = $Text + "`n" + $StdErrText
        }
    }

    $ResultLine = Get-MarkerLine -Text $Text -Prefix 'GPU_RID_RESULT '
    $CountsLine = Get-MarkerLine -Text $Text -Prefix 'GPU_RID_COUNTS '

    $IsSkip = $ResultLine -match 'skip=true'
    $HarnessPass = $ResultLine -match 'pass=true'
    $Leaked = Get-MarkerInt -Line $CountsLine -Field 'leaked'
    $Orphans = Get-MarkerInt -Line $CountsLine -Field 'orphans'
    $Gpu = Get-MarkerInt -Line $CountsLine -Field 'gpu'
    $Loaded = Get-MarkerInt -Line $CountsLine -Field 'loaded'
    $Census = Get-TextureLeakCensus -Text $Text
    $PrewarmTag = Get-MarkerInt -Line $CountsLine -Field 'prewarm_tag'
    $CpuPrewarmStored = Get-MarkerInt -Line $CountsLine -Field 'cpu_prewarm_stored'
    # Proves the teardown_prewarm_pending scenario actually held a GPU RID; -1
    # (absent) means the precondition was never met and a census=0 is vacuous.
    $PrewarmPending = Get-MarkerInt -Line $CountsLine -Field 'prewarm_pending'
    # Preloader refusal warnings: one per entry point (prewarm_async and
    # prewarm_gpu_async), both called once by the scenario while disabled.
    $RefusalWarnings = ([regex]::Matches($Text, 'prewarm(_gpu)?_async skipped')).Count

    $HarnessText = 'n/a'
    if ($ResultLine -ne '') {
        if ($IsSkip) {
            $HarnessText = 'skip'
        } elseif ($HarnessPass) {
            $HarnessText = 'pass'
        } else {
            $HarnessText = 'FAIL'
        }
    }

    $Ok = $false
    $Note = ''
    if ($TimedOut) {
        $Note = "TIMEOUT after $TimeoutSec s"
    } elseif ($ResultLine -eq '') {
        $Note = 'no GPU_RID_RESULT marker; harness did not run (see logs)'
    } elseif ($IsSkip) {
        $AnySkip = $true
        $Note = 'environment skip (no D3D12 GPU decompression)'
    } elseif ($ProbeScenarios -contains $Scenario) {
        if ($Expect -eq 'bug') {
            if ((-not $HarnessPass) -and ($Leaked -ge 1) -and ($Census -ge 1)) {
                $Ok = $true
                $Note = 'bug reproduced (harness red + RID proven alive)'
            } else {
                $Note = 'expected a reproducible leak, saw none'
            }
        } else {
            if ($HarnessPass -and ($Leaked -eq 0) -and ($Census -eq 0)) {
                $Ok = $true
                $Note = 'no leak'
            } else {
                $Note = 'leak still present'
            }
        }
    } elseif ($Scenario -eq 'prewarm_disabled_cache') {
        if ($HarnessPass -and ($PrewarmTag -eq 0) -and ($RefusalWarnings -eq 2) -and ($Census -eq 0) -and ($CpuPrewarmStored -eq 0)) {
            $Ok = $true
            $Note = 'both prewarm paths refused (2 warnings); capped slice stored 0'
        } else {
            $Note = "tag=$PrewarmTag warnings=$RefusalWarnings census=$Census cpu_stored=$CpuPrewarmStored (want 0/2/0/0)"
        }
    } elseif ($Scenario -eq 'census_control') {
        # Deliberate leak: the census MUST see it, in either expectation mode.
        if ($Census -ge 1) {
            $Ok = $true
            $Note = 'exit census reported the deliberate leak (oracle live)'
        } else {
            $Note = 'exit census did not report the deliberate leak - census assertions are unusable here'
        }
    } elseif ($Scenario -eq 'teardown_prewarm_pending') {
        if ($Expect -eq 'bug') {
            if ($HarnessPass -and ($Census -ge 1) -and ($PrewarmPending -ge 1)) {
                $Ok = $true
                $Note = "leak reproduced (census=$Census pending=$PrewarmPending)"
            } else {
                $Note = "expected a leaked preloader RID (census=$Census pending=$PrewarmPending harness_pass=$HarnessPass)"
            }
        } else {
            if ($HarnessPass -and ($Census -eq 0) -and ($PrewarmPending -ge 1)) {
                $Ok = $true
                $Note = "no leak (census=0, pending=$PrewarmPending)"
            } else {
                $Note = "census=$Census pending=$PrewarmPending (want 0 and >=1)"
            }
        }
    } else {
        $Ok = $HarnessPass
        if ($Ok -and ($Scenario -eq 'teardown_immediate') -and ($Census -ne 0)) {
            # The harness cannot read its own exit census, so this scenario is
            # decided here: a texture dropped just before quit must not leave a
            # RID behind for teardown to find.
            $Ok = $false
            $Note = 'exit census reported leaked texture RIDs'
        } elseif ($Ok) {
            if ($Scenario -eq 'oracle') {
                $Note = 'detector sees frees and live RIDs'
            } elseif ($Scenario -eq 'reuse') {
                $Note = 'GPU Texture2DRD path alive'
            } elseif ($Scenario -eq 'teardown_immediate') {
                $Note = 'drop-then-quit leaves no RID for teardown'
            } elseif ($Scenario -eq 'deferral_retry') {
                $Note = 'deferred while in flight, freed by the frame retry'
            } else {
                $Note = 'green once RIDs are released'
            }
        } else {
            $Note = 'harness failed'
        }
    }

    if (-not $Ok) {
        $AnyFail = $true
    }

    $Rows += [pscustomobject]@{
        Scenario = $Scenario
        Harness  = $HarnessText
        Gpu      = $Gpu
        Loaded   = $Loaded
        Orphans  = $Orphans
        Leaked   = $Leaked
        Census   = $Census
        Exit     = $ExitCode
        Ok       = $Ok
        Note     = $Note
    }
}

Write-Output ('{0,-16} {1,-8} {2,-5} {3,-7} {4,-8} {5,-7} {6,-7} {7,-5} {8}' -f `
        'scenario', 'harness', 'gpu', 'loaded', 'orphans', 'leaked', 'census', 'exit', 'verdict')
foreach ($Row in $Rows) {
    $Verdict = 'FAIL'
    if ($Row.Ok) {
        $Verdict = 'OK'
    }
    Write-Output ('{0,-16} {1,-8} {2,-5} {3,-7} {4,-8} {5,-7} {6,-7} {7,-5} {8} ({9})' -f `
            $Row.Scenario, $Row.Harness, $Row.Gpu, $Row.Loaded, $Row.Orphans, $Row.Leaked, $Row.Census, $Row.Exit, $Verdict, $Row.Note)
}

Write-Output ''
Write-Output 'Legend: gpu/loaded = loads classified; orphans = RIDs still alive right after the texture was dropped;'
Write-Output '        leaked = orphans this run did not free; census = engine "RIDs of type Texture were leaked" count at exit.'
Write-Output '        reuse leaks are not asserted: store() makes textures engine-resident (documented upstream behaviour).'
Write-Output ''

if ($AnySkip -and -not $AnyFail) {
    Write-Output 'RESULT: SKIP - no D3D12 GPU decompression in this environment (needs a windowed D3D12 run).'
    exit 2
}
if ($AnyFail) {
    if ($Expect -eq 'fixed') {
        Write-Output 'RESULT: FAIL - the uncached GPU path still orphans texture RIDs. This is the expected pre-fix state; run with -Expect bug to assert the repro.'
    } else {
        Write-Output 'RESULT: FAIL - the repro did not show a leak (or an invariant scenario broke). Is the fix already applied?'
    }
    exit 1
}
if ($Expect -eq 'bug') {
    Write-Output 'RESULT: PASS - bug reproduced: uncached loads orphan GPU texture RIDs and the engine census agrees.'
} else {
    Write-Output 'RESULT: PASS - no orphaned texture RIDs on the uncached load paths; GPU path still alive.'
}
exit 0
