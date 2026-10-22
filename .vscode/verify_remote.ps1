<#
.SYNOPSIS
    Device B verification helper for .vscode/build_remote.ps1. Runs ON Device B.

.DESCRIPTION
    Invoked via ssh WITHOUT any quoted arguments (all paths are spaceless),
    so there is no nested-quoting fragility:
      powershell -NoProfile -ExecutionPolicy Bypass -File <dir>\verify_remote.ps1 -RemoteDir <dir> [-ManifestName sync_manifest.txt] [-Artifacts]

    Manifest mode re-hashes a Device A manifest (lines "relpath|SHA256") and
    prints MANIFEST_LINES=N plus SYNC_MANIFEST_OK or MISSING:/HASH: lines.
    Artifact mode prints ARTIFACT:name|SHA256 lines for shippable binaries.
    Every run also ends with VERIFY_TIMING:TOTAL_MS=..;TOUCH_MS=..;HASH_MS=..
    so build_remote.ps1 can show where the Device B time actually goes.
#>
param(
    [string]$RemoteDir = "",
    [string]$ManifestName = "",
    [switch]$Artifacts,
    [switch]$TouchTests,
    [switch]$CleanOrphans,
    [switch]$ResetMarker
)

$ErrorActionPreference = "Stop"

# Wall-clock split for build_remote.ps1 [3b/6]. Started right after script load,
# so TOTAL_MS - TOUCH_MS - HASH_MS is process startup + manifest read/parse.
$verifyTimer = [System.Diagnostics.Stopwatch]::StartNew()
$touchMs = 0
$hashMs = 0

if ([string]::IsNullOrEmpty($RemoteDir)) {
    Write-Output "VERIFY_ERROR:Missing -RemoteDir"
    exit 2
}

# Scoped process cleanup: kill any orphaned build processes whose CommandLine
# explicitly contains this specific $RemoteDir. This prevents file lock conflicts
# on .sconsign, object files, or DLLs caused by previous interrupted builds, while
# strictly avoiding killing unrelated processes or builds in other directories.
# NOTE: server-side -Filter keeps this fast (a full Win32_Process scan with
# CommandLine for every process costs 20-60s cold). Only cl/link/scons are
# targeted; python/cmd are intentionally excluded (too common, risky /T kill).
if ($CleanOrphans) {
    $killed = 0
    try {
        $procs = Get-CimInstance Win32_Process -Filter "Name='cl.exe' OR Name='link.exe' OR Name='scons.exe'" -ErrorAction SilentlyContinue
        $normRemoteDir = $RemoteDir.TrimEnd('\', '/')
        $targetProcs = @($procs | Where-Object {
            if (-not $_.CommandLine) { return $false }
            return ($_.CommandLine.IndexOf($normRemoteDir, [System.StringComparison]::OrdinalIgnoreCase) -ge 0)
        })

        foreach ($p in $targetProcs) {
            if ($p.ProcessId -eq $PID -or $p.ProcessId -le 4) { continue }
            try {
                Start-Process "taskkill.exe" -ArgumentList @("/F", "/T", "/PID", $p.ProcessId) -NoNewWindow -Wait -ErrorAction SilentlyContinue | Out-Null
                Write-Output ("KILLED_ORPHAN:" + $p.ProcessId + "|" + $p.Name)
                $killed++
            } catch {
                $null = $_
            }
        }
    } catch {
        Write-Output ("CLEAN_ORPHANS_ERROR:" + $_.Exception.Message)
    }
    Write-Output ("CLEANED_ORPHANS_COUNT=" + $killed)
}

# Marker reset: delete the SCons success marker in-process so the caller needs
# no extra SSH round-trips (del + type check). A stale marker fakes success
# (observed: del silently failed on a locked file), so the deletion is verified
# and reported explicitly.
if ($ResetMarker) {
    $markerPath = Join-Path $RemoteDir "gdextest_scons.ok"
    try {
        Remove-Item -LiteralPath $markerPath -Force -ErrorAction SilentlyContinue
    } catch {
        $null = $_
    }
    if (Test-Path -LiteralPath $markerPath) {
        Write-Output "MARKER_STUCK"
    } else {
        Write-Output "MARKER_RESET_OK"
    }
}


# Rebuild forcing: bump the mtime of every test source so SCons cannot
# consider a changed/new tests/*.cpp up to date (observed: edited suites
# silently skipped until touched). Test TUs only (fast); extension sources
# have never shown the same staleness.
if ($TouchTests) {
    $touchTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $testsDir = Join-Path $RemoteDir "tests"
    $touched = 0
    foreach ($testFile in (Get-ChildItem -LiteralPath $testsDir -Filter *.cpp -Recurse -File -ErrorAction SilentlyContinue)) {
        try {
            $testFile.LastWriteTime = Get-Date
            $touched++
        } catch {
            Write-Output ("VERIFY_WARNING:Could not touch " + $testFile.FullName + " (" + $_.Exception.Message + ")")
        }
    }
    $touchTimer.Stop()
    $touchMs = [int][math]::Round($touchTimer.Elapsed.TotalMilliseconds)
    Write-Output ("TOUCHED_TESTS=" + $touched)
    if ($touched -eq 0) {
        Write-Output "VERIFY_ERROR:No test sources found under tests/"
        exit 2
    }
}

if ($ManifestName -ne "") {
    $hashTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $manifestPath = Join-Path $RemoteDir $ManifestName
    $lines = @(Get-Content -LiteralPath $manifestPath)
    Write-Output ("MANIFEST_LINES=" + $lines.Count)
    $bad = @()
    # One SHA256 instance for the whole manifest. Get-FileHash costs ~1.3 ms per
    # call in Windows PowerShell (cmdlet + provider + FileInfo rebuild); hashing
    # the raw stream is ~0.08 ms. File.Exists replaces Test-Path for the same
    # reason -- at ~600 entries those two calls alone are most of the wall time.
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($entry in $lines) {
            $sep = $entry.IndexOf("|")
            if ($sep -lt 0) { continue }
            $rel = $entry.Substring(0, $sep)
            $expected = $entry.Substring($sep + 1)
            $full = Join-Path $RemoteDir ($rel.Replace("/", [System.IO.Path]::DirectorySeparatorChar))
            if (-not [System.IO.File]::Exists($full)) {
                $bad += ("MISSING:" + $rel)
                continue
            }
            try {
                $stream = [System.IO.File]::Open($full, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
                try {
                    $actual = [System.BitConverter]::ToString($sha256.ComputeHash($stream)).Replace("-", "")
                } finally {
                    $stream.Dispose()
                }
            } catch {
                $bad += ("HASH:" + $rel)
                continue
            }
            if ($actual -ne $expected) {
                $bad += ("HASH:" + $rel)
            }
        }
    } finally {
        $sha256.Dispose()
    }
    $hashTimer.Stop()
    $hashMs = [int][math]::Round($hashTimer.Elapsed.TotalMilliseconds)
    if ($bad.Count -eq 0) {
        Write-Output "SYNC_MANIFEST_OK"
    } else {
        foreach ($b in $bad) { Write-Output $b }
    }
}

if ($Artifacts) {
    $artifactDirs = @(
        (Join-Path $RemoteDir "project/addons/directstorage_accelerator/bin"),
        (Join-Path $RemoteDir "bin")
    )
    foreach ($artifactDir in $artifactDirs) {
        $files = Get-ChildItem -LiteralPath $artifactDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -eq ".dll" -or $_.Name -eq "build_manifest.json" }
        foreach ($artifactFile in $files) {
            Write-Output ("ARTIFACT:" + $artifactFile.Name + "|" + (Get-FileHash -LiteralPath $artifactFile.FullName -Algorithm SHA256).Hash)
        }
    }
}

$verifyTimer.Stop()
Write-Output ("VERIFY_TIMING:TOTAL_MS=" + [int][math]::Round($verifyTimer.Elapsed.TotalMilliseconds) + ";TOUCH_MS=" + $touchMs + ";HASH_MS=" + $hashMs)
