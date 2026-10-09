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
#>
param(
    [string]$RemoteDir = "",
    [string]$ManifestName = "",
    [switch]$Artifacts,
    [switch]$TouchTests,
    [switch]$CleanOrphans
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($RemoteDir)) {
    Write-Output "VERIFY_ERROR:Missing -RemoteDir"
    exit 2
}

# Scoped process cleanup: kill any orphaned build processes whose CommandLine
# explicitly contains this specific $RemoteDir. This prevents file lock conflicts
# on .sconsign, object files, or DLLs caused by previous interrupted builds, while
# strictly avoiding killing unrelated processes or builds in other directories.
if ($CleanOrphans) {
    $killed = 0
    try {
        $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue
        $normRemoteDir = $RemoteDir.TrimEnd('\', '/')
        $targetProcs = @($procs | Where-Object {
            if (-not $_.CommandLine) { return $false }
            $cmd = $_.CommandLine
            $hasRemoteDir = $cmd.IndexOf($normRemoteDir, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
            if (-not $hasRemoteDir) { return $false }

            $name = $_.Name.ToLower()
            $isBuildTool = ($name -in @("cl.exe", "link.exe", "scons.exe", "python.exe", "python3.exe", "cmd.exe")) -and
                           ($cmd -like "*scons*" -or $name -in @("cl.exe", "link.exe"))
            return $isBuildTool
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


# Rebuild forcing: bump the mtime of every test source so SCons cannot
# consider a changed/new tests/*.cpp up to date (observed: edited suites
# silently skipped until touched). Test TUs only (fast); extension sources
# have never shown the same staleness.
if ($TouchTests) {
    $testsDir = Join-Path $RemoteDir "tests"
    $touched = 0
    foreach ($testFile in (Get-ChildItem -LiteralPath $testsDir -Filter *.cpp -Recurse -File -ErrorAction SilentlyContinue)) {
        $testFile.LastWriteTime = Get-Date
        $touched++
    }
    Write-Output ("TOUCHED_TESTS=" + $touched)
    if ($touched -eq 0) {
        Write-Output "VERIFY_ERROR:No test sources found under tests/"
        exit 2
    }
}

if ($ManifestName -ne "") {
    $manifestPath = Join-Path $RemoteDir $ManifestName
    $lines = @(Get-Content -LiteralPath $manifestPath)
    Write-Output ("MANIFEST_LINES=" + $lines.Count)
    $bad = @()
    foreach ($entry in $lines) {
        $sep = $entry.IndexOf("|")
        if ($sep -lt 0) { continue }
        $rel = $entry.Substring(0, $sep)
        $expected = $entry.Substring($sep + 1)
        $full = Join-Path $RemoteDir ($rel.Replace("/", [System.IO.Path]::DirectorySeparatorChar))
        if (-not (Test-Path -LiteralPath $full)) {
            $bad += ("MISSING:" + $rel)
        } elseif ((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ne $expected) {
            $bad += ("HASH:" + $rel)
        }
    }
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
