<#
.SYNOPSIS
    Cross-runner concurrency guard: one gdextest run per workspace at a time.

.DESCRIPTION
    run_d3d12_windowed.ps1 and run_gdextest_local.ps1 both drive Godot against
    the SAME fixture project (build/gdextest/project) of the same checkout, so
    two concurrent runs destroy each other instead of merely contending:

      * The windowed runner copies bin/libdsgd_tests.dll and dstorage*.dll into
        the fixture addons bin with -Force. A live engine has the test library
        mapped from there, so the copy fails with a bare Win32 sharing
        violation under $ErrorActionPreference = "Stop" - an error that reads
        like a broken build, not like "a run is already in flight".
      * Both runners write the same result JSON, and the windowed runner writes
        fixed redirect logs (.vscode/d3d12_stdout.log, d3d12_stderr.log) and
        clears them before launching. The second run deletes the first run's
        artifacts.
      * run_gdextest_local.ps1 regenerates the fixture (generate_fixture.py
        rewrites addons/gdextest/gdextest.gdextension and its bin/), and its
        Stop-OrphanFixtureProcess taskkills every Godot process whose command
        line names this worktree's fixture path - i.e. it kills a live windowed
        run mid-suite.

    The guard is a workspace-keyed named kernel mutex held for the whole run, so
    a second run fails in under a second instead of corrupting the first. The
    kernel releases it when the owning process terminates, so there is no lock
    file to go stale and none to clean up by hand - which matters here because
    both runners are designed to be killed by their own watchdog.

    Deliberately NOT the build guard (dsgd_build_*, see .vscode/build_remote.ps1):
    a run touches neither the SCons object tree, nor the gdextest_scons.ok
    marker, nor src/gen, nor any compiler process, so a run must not block
    behind a multi-minute remote build.

    Known gaps (the callers keep their existing orphan checks for these):
      * If the runner's PowerShell process is killed while the Godot child
        survives, the mutex is released while the engine still holds the
        fixture DLLs. Caller-side stale-artifact checks still detect that.
      * A manually opened Godot editor on the fixture project takes no part in
        this guard.
#>

# Workspace-keyed guard name. Normalization matches build_remote.ps1's
# Get-BuildGuardName so one checkout always maps to one object regardless of how
# the path was spelled (trailing separator, drive-letter case, .. segments).
function Get-RunGuardName {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Root)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Root.TrimEnd('\', '/').ToLowerInvariant()))
    }
    finally {
        $sha.Dispose()
    }
    return "dsgd_run_" + ([System.BitConverter]::ToString($digest[0..7])).Replace('-', '')
}

# Returns Owned = $true (acquired), $false (held by another run), or $null (the
# guard object could not be created at all - the caller warns instead of
# bricking test runs on a policy quirk).
function Enter-RunGuard {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Name)
    # Global\ also covers a run started in another Windows session; the
    # per-session namespace is the fallback when policy denies Global objects.
    foreach ($prefix in @('Global\', '')) {
        $mutex = $null
        try {
            $mutex = New-Object System.Threading.Mutex($false, ($prefix + $Name)) -ErrorAction Stop
        }
        catch {
            continue
        }
        $owned = $false
        try {
            $owned = $mutex.WaitOne(0)
        }
        catch [System.Threading.AbandonedMutexException] {
            # The previous owner died mid-run; the kernel already released it.
            $owned = $true
        }
        if ($owned) {
            return [pscustomobject]@{ Owned = $true; Mutex = $mutex; FullName = ($prefix + $Name) }
        }
        $mutex.Dispose()
        return [pscustomobject]@{ Owned = $false; Mutex = $null; FullName = ($prefix + $Name) }
    }
    return [pscustomobject]@{ Owned = $null; Mutex = $null; FullName = $Name }
}
