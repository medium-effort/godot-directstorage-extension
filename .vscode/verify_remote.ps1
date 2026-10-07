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
    [switch]$Artifacts
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($RemoteDir)) {
    Write-Output "VERIFY_ERROR:Missing -RemoteDir"
    exit 2
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
