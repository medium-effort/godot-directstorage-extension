<#
.SYNOPSIS
    Verifies that the vendored Monocypher sources are the bytes of the pinned upstream release.

.DESCRIPTION
    Reads thirdparty/monocypher/VERSION.txt and compares the SHA-256 of every file it pins with
    the bytes on disk. The record is the only place that names which upstream release the
    vendored crypto came from: the sources themselves carry Monocypher's unexpanded
    "version __git__" banner (upstream ships that in git too, it is substituted only in release
    tarballs), so hashing is the only way to detect a local edit or an unnoticed upgrade.

    The ".txt" suffix is load-bearing, not cosmetic: thirdparty/monocypher is on CPPPATH, and on
    the case-insensitive filesystems macOS and Windows use, a file named exactly "VERSION" is
    found instead of the C++ standard header <version> that libc++ includes from <limits>, which
    breaks every C++ translation unit on macOS. Do not rename it back.

    VERSION.txt format: '#' metadata comments plus sha256sum-compatible "<hex>  <path>" pairs, so
    the pins can be re-verified without this repository's tooling, from the directory:
        sha256sum -c VERSION.txt

    Output is plain (no colours; the exit code is the verdict) so .vscode/build_remote.ps1 can
    capture and re-emit it without polluting stdout in -Json mode.

.PARAMETER Root
    Repository root. Defaults to the parent directory of this script.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$Root = ""
)

$ErrorActionPreference = "Stop"

$VendoredDir = "thirdparty/monocypher"
$VersionFile = "VERSION.txt"
$ExpectedFiles = @("LICENSE", "monocypher.c", "monocypher.h")

function Write-ProvenanceError {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Output ("[provenance] ERROR: " + $Message)
}

function Get-FileSha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha.Dispose()
    }
}

try {
    if (-not $Root) {
        $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    }
    $dir = Join-Path $Root $VendoredDir
    $versionPath = Join-Path $dir $VersionFile

    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        Write-ProvenanceError ("vendored directory not found: " + $VendoredDir)
        exit 1
    }
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        Write-ProvenanceError ("no provenance record: " + $VendoredDir + "/" + $VersionFile + " is missing, so the vendored crypto cannot be tied to an upstream release.")
        exit 1
    }

    $tag = $null
    $pinned = [ordered]@{}
    foreach ($line in [System.IO.File]::ReadAllLines($versionPath, [System.Text.UTF8Encoding]::new($false))) {
        $trimmed = $line.Trim()
        if ($trimmed -eq "") { continue }
        if ($trimmed.StartsWith("#")) {
            if ($trimmed -match '^#\s*tag:\s*(\S.*?)\s*$') { $tag = $matches[1] }
            continue
        }
        if ($trimmed -match '^([0-9A-Fa-f]{64})\s+\*?(.+?)\s*$') {
            $name = $matches[2]
            if ($pinned.Contains($name)) {
                Write-ProvenanceError ("duplicate pin for '" + $name + "' in " + $VendoredDir + "/" + $VersionFile + ".")
                exit 1
            }
            $pinned[$name] = $matches[1].ToLowerInvariant()
            continue
        }
        Write-ProvenanceError ("unparsable line in " + $VendoredDir + "/" + $VersionFile + ": " + $trimmed)
        exit 1
    }

    if (-not $tag) {
        Write-ProvenanceError ("no pinned upstream tag in " + $VendoredDir + "/" + $VersionFile + " (add '# tag: <x.y.z>').")
        exit 1
    }
    if ($tag -notmatch '^\d+\.\d+\.\d+$') {
        Write-ProvenanceError ("no pinned upstream tag in " + $VendoredDir + "/" + $VersionFile + " (found '" + $tag + "').")
        exit 1
    }

    foreach ($name in $ExpectedFiles) {
        if (-not $pinned.Contains($name)) {
            Write-ProvenanceError ($VendoredDir + "/" + $VersionFile + " does not pin '" + $name + "'; the record and the directory disagree.")
            exit 1
        }
    }
    foreach ($name in $pinned.Keys) {
        if ($ExpectedFiles -notcontains $name) {
            Write-ProvenanceError ($VendoredDir + "/" + $VersionFile + " pins an unexpected file '" + $name + "'.")
            exit 1
        }
    }

    foreach ($name in $ExpectedFiles) {
        $path = Join-Path $dir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Write-ProvenanceError ("missing pinned file '" + $name + "' in " + $VendoredDir + ".")
            exit 1
        }
        $actual = Get-FileSha256Hex -Path $path
        if ($actual -ne $pinned[$name]) {
            Write-ProvenanceError ("hash mismatch for " + $VendoredDir + "/" + $name + " - pinned " + $pinned[$name] + ", found " + $actual + " (local edit of vendored code, or a stale record).")
            exit 1
        }
    }

    $unexpected = @()
    $sourceExtensions = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".def", ".s", ".asm")
    # SCons writes <source>.<platform>.<target>.<arch>.obj next to the source, so "any file that
    # is not pinned" would fail every build after the first. What matters is an addition that can
    # change the compiled code: a tracked file, or a file with a source extension (an untracked
    # patched copy would join the gdextest build, whose glob is thirdparty/monocypher/*.c).
    $tracked = @()
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        try {
            $tracked = @(& $git.Source -C $Root ls-files -- $VendoredDir 2>$null)
            if ($LASTEXITCODE -ne 0) { $tracked = @() }
        }
        catch {
            $tracked = @()
        }
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $dir -File -Force | Sort-Object Name)) {
        if ($file.Name -eq $VersionFile -or $ExpectedFiles -contains $file.Name) { continue }
        $relative = $VendoredDir + "/" + $file.Name
        $extension = [System.IO.Path]::GetExtension($file.Name)
        if (($sourceExtensions -contains $extension.ToLowerInvariant()) -or ($tracked -contains $relative)) {
            $unexpected += $file.Name
        }
    }
    if ($unexpected.Count -gt 0) {
        foreach ($extra in $unexpected) {
            Write-ProvenanceError ("unexpected file '" + $extra + "' in " + $VendoredDir + "; a vendored addition must update " + $VersionFile + " deliberately.")
        }
        exit 1
    }

    Write-Output ("[provenance] monocypher " + $tag + ": " + $ExpectedFiles.Count + "/" + $ExpectedFiles.Count + " file(s) match the pinned SHA-256, 0 unexpected file(s).")
    exit 0
}
catch {
    Write-ProvenanceError ("check could not complete: " + $_.Exception.Message)
    exit 1
}
