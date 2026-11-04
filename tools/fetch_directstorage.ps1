<#
.SYNOPSIS
    Fetch the Microsoft DirectStorage SDK runtime files from the pinned NuGet package.

.DESCRIPTION
    The DirectStorage headers are MIT and stay in thirdparty/directstorage/include.
    The runtime DLLs and the import library are covered by the Microsoft package
    EULA; distributable_files.txt lists only dstorage.dll and dstoragecore.dll, and
    native/lib/dstorage.lib is not in that list. Neither is committed to this
    repository, so every Windows build path fetches the pinned package here.

    This script is contributor/CI tooling. It is never invoked by SCons or
    build_remote.ps1 (those only check that the import library exists and fail with
    an instruction to run this script).

    Flow:
      1. Read tools/directstorage_versions.json, which pins the package URL,
         SHA-512 (Microsoft's published packageHash) and size.
      2. Download to .cache/directstorage/packages/<version>.nupkg (reused when the
         bytes still match).
      3. Verify size and SHA-512 before anything is extracted.
      4. Extract the exact LICENSE.txt and show it in a separate PowerShell window
         (tools/show_directstorage_license.ps1).
      5. Record accept/decline per version in .cache/directstorage/licenses.json.
         An accepted version whose hash still matches skips the prompt; a declined
         version refuses until -ForcePrompt or -ResetLicense.
      6. On accept, extract the runtime DLLs to thirdparty/directstorage/bin/x64/
         and, for 1.3.x, dstorage.lib to thirdparty/directstorage/lib/x64/.
         The DLLs are also copied into the addon bin unless -SkipAddonCopy.

    License acceptance is explicit. There is no automatic accept; CI must pass
    -Decision Accept, which makes the acceptance visible in the workflow log.

.PARAMETER Version
    Key from tools/directstorage_versions.json. Defaults to 1.3.0, the build
    baseline. 1.4 fetches runtime DLLs only and leaves the 1.3 import library and
    headers untouched.

.PARAMETER Decision
    Non-interactive seam for CI and tests: Accept or Decline without opening the
    license window. The decision is still recorded in the per-version cache.

.PARAMETER ForcePrompt
    Ask again even when this version was already accepted or declined.

.PARAMETER ResetLicense
    Delete this version's cached decision (and the stale decision file) before
    running. Implies a fresh prompt unless -Decision is also passed.

.PARAMETER SkipAddonCopy
    Do not copy the runtime DLLs into project/addons/directstorage_accelerator/bin/.
    Useful when an editor holds them and the in-editor installer should apply the
    update on the next start instead.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory.

.PARAMETER CacheDir
    License/download cache. Defaults to <RepoRoot>/.cache/directstorage.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1 -Version 1.4.0-preview2-2606.904

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1 -Version 1.3.0 -Decision Accept
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Interactive contributor/CI tool; console output is the interface.')]
[CmdletBinding()]
param(
    [string]$Version = '1.3.0',
    [ValidateSet('', 'Accept', 'Decline')][string]$Decision = '',
    [switch]$ForcePrompt,
    [switch]$ResetLicense,
    [switch]$SkipAddonCopy,
    [string]$RepoRoot = '',
    [string]$CacheDir = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "[directstorage] $Text" -ForegroundColor Cyan
}

function Write-Info {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "  $Text"
}

function Write-Warn {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "WARNING: $Text" -ForegroundColor Yellow
}

function Write-Failure {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [int]$Code = 2
    )
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit $Code
}

function Get-Sha512Base64 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [System.Security.Cryptography.SHA512]::Create()
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        try {
            $digest = $sha.ComputeHash($stream)
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha.Dispose()
    }
    return [Convert]::ToBase64String($digest)
}

function Test-Package {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$Size,
        [Parameter(Mandatory = $true)][string]$Sha512
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    if ((Get-Item -LiteralPath $Path).Length -ne $Size) {
        return $false
    }
    return (Get-Sha512Base64 -Path $Path) -eq $Sha512
}

function Read-LicenseCache {
    param([Parameter(Mandatory = $true)][string]$Path)

    $cache = @{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $cache
    }
    try {
        $raw = [System.IO.File]::ReadAllText($Path, [System.Text.UTF8Encoding]::new($false))
        $parsed = ConvertFrom-Json -InputObject $raw
    }
    catch {
        Write-Warn "License cache is not valid JSON; ignoring it: $Path"
        return $cache
    }
    if ($null -eq $parsed) {
        return $cache
    }
    foreach ($property in $parsed.PSObject.Properties) {
        $cache[$property.Name] = $property.Value
    }
    return $cache
}

function Save-LicenseCache {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][hashtable]$Cache
    )

    $ordered = [ordered]@{}
    foreach ($key in ($Cache.Keys | Sort-Object)) {
        $value = $Cache[$key]
        $ordered[$key] = [ordered]@{
            accepted   = [bool]$value.accepted
            sha512     = [string]$value.sha512
            decided_at = [string]$value.decided_at
        }
    }
    $json = $ordered | ConvertTo-Json -Depth 6
    $parent = Split-Path -Path $Path -Parent
    if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function Save-DecisionRecord {
    param(
        [Parameter(Mandatory = $true)][bool]$Accepted,
        [Parameter(Mandatory = $true)][string]$Sha512,
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][hashtable]$Cache
    )

    $Cache[$Version] = [pscustomobject]@{
        accepted   = $Accepted
        sha512     = $Sha512
        decided_at = (Get-Date).ToUniversalTime().ToString('o')
    }
    Save-LicenseCache -Path $CachePath -Cache $Cache
}

# --- Resolve paths -----------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Path $PSScriptRoot -Parent
}
if (-not (Test-Path -LiteralPath $RepoRoot -PathType Container)) {
    Write-Failure "Repository root not found: $RepoRoot"
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path.TrimEnd('\', '/')

if ([string]::IsNullOrWhiteSpace($CacheDir)) {
    $CacheDir = Join-Path $RepoRoot '.cache\directstorage'
}
elseif (-not [System.IO.Path]::IsPathRooted($CacheDir)) {
    $CacheDir = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $CacheDir))
}

$ManifestPath = Join-Path $PSScriptRoot 'directstorage_versions.json'
$ViewerPath = Join-Path $PSScriptRoot 'show_directstorage_license.ps1'
foreach ($required in @($ManifestPath, $ViewerPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Write-Failure "Required file missing: $required"
    }
}

$PackagesDir = Join-Path $CacheDir 'packages'
$CacheJson = Join-Path $CacheDir 'licenses.json'
$NupkgPath = Join-Path $PackagesDir ($Version + '.nupkg')
$LicensePath = Join-Path $CacheDir ('license-' + $Version + '.txt')
$DecisionPath = Join-Path $CacheDir ('decision-' + $Version + '.txt')

# --- Read and validate the manifest -----------------------------------------
$manifestText = [System.IO.File]::ReadAllText($ManifestPath, [System.Text.UTF8Encoding]::new($false))
$manifest = ConvertFrom-Json -InputObject $manifestText
$manifestProperty = $manifest.PSObject.Properties[$Version]
if ($null -eq $manifestProperty) {
    $known = @($manifest.PSObject.Properties | ForEach-Object { $_.Name }) -join ', '
    Write-Failure "Unknown DirectStorage version '$Version'. Known versions: $known"
}
$package = $manifestProperty.Value
$url = [string]$package.url
$sha512 = [string]$package.sha512
$size = [long]$package.size
$importLib = [bool]$package.import_lib
if ([string]::IsNullOrWhiteSpace($url) -or [string]::IsNullOrWhiteSpace($sha512) -or $size -le 0) {
    Write-Failure "Manifest entry for '$Version' is incomplete (url, sha512 and size are required)."
}

$previewTag = ''
if ([bool]$package.preview) {
    $previewTag = ' (preview)'
}
Write-Step "DirectStorage $Version$previewTag"

# --- Cached decision ---------------------------------------------------------
$licenseCache = Read-LicenseCache -Path $CacheJson
if ($ResetLicense) {
    if ($licenseCache.ContainsKey($Version)) {
        $licenseCache.Remove($Version)
        Save-LicenseCache -Path $CacheJson -Cache $licenseCache
    }
    if (Test-Path -LiteralPath $DecisionPath) {
        Remove-Item -LiteralPath $DecisionPath -Force
    }
    Write-Info "Cleared the cached license decision for $Version."
}

$cachedEntry = $licenseCache[$Version]
$cachedHashMatches = ($null -ne $cachedEntry) -and ([string]$cachedEntry.sha512 -eq $sha512)
$cachedAccepted = $cachedHashMatches -and [bool]$cachedEntry.accepted
$cachedDeclined = $cachedHashMatches -and -not [bool]$cachedEntry.accepted

if ($cachedDeclined -and -not $ForcePrompt -and [string]::IsNullOrWhiteSpace($Decision)) {
    Write-Failure "The $Version license was declined previously. Pass -ResetLicense (or -ForcePrompt) to decide again. Nothing was installed." 1
}

# --- Download and verify -----------------------------------------------------
if (-not (Test-Path -LiteralPath $PackagesDir -PathType Container)) {
    [System.IO.Directory]::CreateDirectory($PackagesDir) | Out-Null
}
if (-not (Test-Path -LiteralPath $CacheDir -PathType Container)) {
    [System.IO.Directory]::CreateDirectory($CacheDir) | Out-Null
}

if (Test-Package -Path $NupkgPath -Size $size -Sha512 $sha512) {
    Write-Info "Using cached package: $NupkgPath"
}
else {
    if (Test-Path -LiteralPath $NupkgPath) {
        Remove-Item -LiteralPath $NupkgPath -Force
    }
    $tempPath = $NupkgPath + '.tmp'
    if (Test-Path -LiteralPath $tempPath) {
        Remove-Item -LiteralPath $tempPath -Force
    }

    Write-Info "Downloading $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $tempPath -UseBasicParsing
    }
    catch {
        if (Test-Path -LiteralPath $tempPath) {
            Remove-Item -LiteralPath $tempPath -Force
        }
        Write-Failure "Download failed: $($_.Exception.Message)"
    }

    if ((Get-Item -LiteralPath $tempPath).Length -ne $size) {
        $actualSize = (Get-Item -LiteralPath $tempPath).Length
        Remove-Item -LiteralPath $tempPath -Force
        Write-Failure "Package size mismatch: got $actualSize bytes, expected $size."
    }
    $actualHash = Get-Sha512Base64 -Path $tempPath
    if ($actualHash -ne $sha512) {
        Remove-Item -LiteralPath $tempPath -Force
        Write-Failure "Package SHA-512 does not match the pinned hash; download discarded."
    }
    Move-Item -LiteralPath $tempPath -Destination $NupkgPath -Force
    Write-Info "Verified SHA-512 and size."
}

# --- Extract the exact package license --------------------------------------
if (-not ('System.IO.Compression.ZipFile' -as [type])) {
    # Windows PowerShell 5.1 needs the assembly loaded; PowerShell 7 already has it,
    # and Add-Type -AssemblyName for it fails there.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
}
$zip = [System.IO.Compression.ZipFile]::OpenRead($NupkgPath)
$licenseEntry = $zip.GetEntry('LICENSE.txt')
if ($null -eq $licenseEntry) {
    $zip.Dispose()
    Write-Failure "Package $Version has no LICENSE.txt; refusing to install it."
}
[System.IO.Compression.ZipFileExtensions]::ExtractToFile($licenseEntry, $LicensePath, $true)
$zip.Dispose()
if (-not (Test-Path -LiteralPath $LicensePath -PathType Leaf)) {
    Write-Failure "Could not extract the package license to $LicensePath"
}
$licenseLength = (Get-Item -LiteralPath $LicensePath).Length
if ($licenseLength -le 0) {
    Write-Failure "Extracted license is empty: $LicensePath"
}
Write-Info "Package license: $LicensePath ($licenseLength bytes)"

# --- Decide ------------------------------------------------------------------
$accepted = $false
if (-not $ForcePrompt -and $cachedAccepted) {
    Write-Info "License already accepted for $Version (hash matches); skipping the prompt."
    $accepted = $true
}
elseif (-not [string]::IsNullOrWhiteSpace($Decision)) {
    if ($Decision -eq 'Accept') {
        $accepted = $true
        Write-Info "License accepted by -Decision."
    }
    else {
        $accepted = $false
        Write-Info "License declined by -Decision."
    }
    $viewerArgs = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -LicensePath "{1}" -Version "{2}" -DecisionPath "{3}" -Decision {4}' -f $ViewerPath, $LicensePath, $Version, $DecisionPath, $Decision
    $viewerProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $viewerArgs -Wait -PassThru -NoNewWindow
    if (-not (Test-Path -LiteralPath $DecisionPath -PathType Leaf)) {
        Write-Failure "The license viewer did not record a decision (exit $($viewerProcess.ExitCode)). Nothing was installed." 1
    }
}
else {
    # A separate console keeps the paged license readable while the fetch log
    # stays in the VS Code task terminal.
    if (Test-Path -LiteralPath $DecisionPath) {
        Remove-Item -LiteralPath $DecisionPath -Force
    }
    Write-Info "Opening the license in a separate PowerShell window..."
    $viewerArgs = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -LicensePath "{1}" -Version "{2}" -DecisionPath "{3}"' -f $ViewerPath, $LicensePath, $Version, $DecisionPath
    $viewerProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $viewerArgs -Wait -PassThru
    $decisionText = ''
    if (Test-Path -LiteralPath $DecisionPath -PathType Leaf) {
        $decisionText = ([System.IO.File]::ReadAllText($DecisionPath, [System.Text.UTF8Encoding]::new($false))).Trim().ToLowerInvariant()
    }
    if ($decisionText -eq 'accept') {
        $accepted = $true
    }
    else {
        if ($decisionText -eq 'decline') {
            Write-Info "License declined; nothing was installed."
        }
        else {
            Write-Warn "No decision file was written (viewer exit $($viewerProcess.ExitCode)); treating this as a decline, so closing the window can never count as acceptance."
        }
        $accepted = $false
    }
}

Save-DecisionRecord -Accepted $accepted -Sha512 $sha512 -CachePath $CacheJson -Cache $licenseCache
if (-not $accepted) {
    if (Test-Path -LiteralPath $DecisionPath) {
        Remove-Item -LiteralPath $DecisionPath -Force
    }
    Write-Host "Nothing was installed. Re-run with -ResetLicense (or -ForcePrompt) to decide again." -ForegroundColor Yellow
    exit 1
}

# --- Extract runtime payload -------------------------------------------------
$DllTargets = [ordered]@{
    'native/bin/x64/dstorage.dll'     = (Join-Path $RepoRoot 'thirdparty\directstorage\bin\x64\dstorage.dll')
    'native/bin/x64/dstoragecore.dll' = (Join-Path $RepoRoot 'thirdparty\directstorage\bin\x64\dstoragecore.dll')
}
if ($importLib) {
    $DllTargets['native/lib/x64/dstorage.lib'] = (Join-Path $RepoRoot 'thirdparty\directstorage\lib\x64\dstorage.lib')
}

$zip = [System.IO.Compression.ZipFile]::OpenRead($NupkgPath)
foreach ($entryName in $DllTargets.Keys) {
    $entry = $zip.GetEntry($entryName)
    if ($null -eq $entry) {
        $zip.Dispose()
        Write-Failure "Package $Version is missing $entryName"
    }
    $destination = $DllTargets[$entryName]
    $destinationDir = Split-Path -Path $destination -Parent
    if (-not (Test-Path -LiteralPath $destinationDir -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($destinationDir) | Out-Null
    }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
    $length = (Get-Item -LiteralPath $destination).Length
    if ($length -le 0) {
        $zip.Dispose()
        Write-Failure "Extracted an empty file: $destination"
    }
    Write-Info "Extracted $entryName -> $destination ($length bytes)"
}
$zip.Dispose()

if ($importLib) {
    Write-Info "Build baseline updated: headers stay at the committed 1.3 set; the import library is now 1.3."
}
else {
    Write-Info "Runtime DLLs updated. The committed 1.3 headers and import library were left untouched."
}

# --- Optional addon copy -----------------------------------------------------
if (-not $SkipAddonCopy) {
    $addonBin = Join-Path $RepoRoot 'project\addons\directstorage_accelerator\bin'
    if (-not (Test-Path -LiteralPath $addonBin -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($addonBin) | Out-Null
    }
    foreach ($dllName in @('dstorage.dll', 'dstoragecore.dll')) {
        $source = Join-Path $RepoRoot ('thirdparty\directstorage\bin\x64\' + $dllName)
        $destination = Join-Path $addonBin $dllName
        try {
            Copy-Item -LiteralPath $source -Destination $destination -Force
            Write-Info "Copied $dllName -> $destination"
        }
        catch {
            Write-Warn "Could not copy $dllName into the addon bin (an editor may hold it): $($_.Exception.Message). The fetched copy in thirdparty/directstorage/bin/x64 is still valid; restart the editor and re-run the fetch, or use the in-editor runtime manager to install it."
        }
    }
}

Write-Host "DirectStorage $Version is ready." -ForegroundColor Green
exit 0
