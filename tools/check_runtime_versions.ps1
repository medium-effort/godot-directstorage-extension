<#
.SYNOPSIS
    Cross-check the addon's built-in runtime package pin against runtime_versions.json.

.DESCRIPTION
    The baseline runtime (1.3.0) is pinned twice on purpose:

      * project/addons/directstorage_accelerator/runtime_versions.json is the
        maintained table the in-editor installer and tools/fetch_directstorage.ps1
        read.
      * ds_runtime_installer.gd keeps the same entry in FALLBACK_VERSIONS so a
        stripped or partially synced copy of the addon can still install the
        baseline when the JSON is missing or unreadable.

    Duplication is the point, but silent drift is not: a stale JSON would make the
    installer verify against a hash that Microsoft's package no longer matches, and
    the failure would surface at install time as a confusing "SHA-512 does not
    match" error. This check fails the build instead.

    It also validates every manifest entry, because an entry without an https URL,
    a SHA-512 or a positive size is not installable and must not reach the table.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_runtime_versions.ps1
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path.TrimEnd('\', '/')

$manifestPath = [System.IO.Path]::Combine($RepoRoot, 'project', 'addons', 'directstorage_accelerator', 'runtime_versions.json')
$installerPath = [System.IO.Path]::Combine($RepoRoot, 'project', 'addons', 'directstorage_accelerator', 'scripts', 'ds_runtime_installer.gd')
$baselineKey = '1.3.0'

$problems = [System.Collections.Generic.List[string]]::new()

foreach ($required in @($manifestPath, $installerPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Write-Host "[runtime-versions] FAILED: required file missing: $required" -ForegroundColor Red
        exit 1
    }
}

# --- Manifest ---------------------------------------------------------------
try {
    $manifest = [System.IO.File]::ReadAllText($manifestPath, [System.Text.UTF8Encoding]::new($false)) | ConvertFrom-Json
}
catch {
    Write-Host "[runtime-versions] FAILED: runtime_versions.json is not valid JSON: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$entries = @($manifest.PSObject.Properties)
if ($entries.Count -eq 0) {
    $problems.Add('runtime_versions.json has no package entries.')
}

foreach ($entry in $entries) {
    $name = $entry.Name
    $package = $entry.Value
    $url = [string]$package.url
    $sha512 = [string]$package.sha512
    $size = [long]$package.size

    if ([string]::IsNullOrWhiteSpace($url) -or -not $url.StartsWith('https://', [System.StringComparison]::OrdinalIgnoreCase)) {
        $problems.Add("'$name' has no https download URL.")
    }
    if ([string]::IsNullOrWhiteSpace($sha512)) {
        $problems.Add("'$name' has no sha512 (the only integrity check before extraction).")
    }
    if ($size -le 0) {
        $problems.Add("'$name' has no positive size.")
    }
}

$baseline = $entries | Where-Object { $_.Name -eq $baselineKey } | Select-Object -First 1
if ($null -eq $baseline) {
    $problems.Add("runtime_versions.json has no '$baselineKey' entry; the build baseline must always be present.")
}

# --- Built-in fallback in the installer -------------------------------------
$installerText = [System.IO.File]::ReadAllText($installerPath, [System.Text.UTF8Encoding]::new($false))
$fallbackBlock = [regex]::Match(
    $installerText,
    '"' + [regex]::Escape($baselineKey) + '"\s*:\s*\{(?<body>[^}]*)\}'
)
if (-not $fallbackBlock.Success) {
    $problems.Add("ds_runtime_installer.gd has no '$baselineKey' entry in FALLBACK_VERSIONS.")
}
else {
    $body = $fallbackBlock.Groups['body'].Value
    $codeUrl = [regex]::Match($body, '"url"\s*:\s*"([^"]+)"')
    $codeSha = [regex]::Match($body, '"sha512"\s*:\s*"([^"]+)"')
    $codeSize = [regex]::Match($body, '"size"\s*:\s*(\d+)')

    if (-not ($codeUrl.Success -and $codeSha.Success -and $codeSize.Success)) {
        $problems.Add("ds_runtime_installer.gd's '$baselineKey' fallback is missing url, sha512 or size.")
    }
    elseif ($null -ne $baseline) {
        if ($codeUrl.Groups[1].Value -ne [string]$baseline.Value.url) {
            $problems.Add("'$baselineKey' url differs: manifest=$($baseline.Value.url) code=$($codeUrl.Groups[1].Value)")
        }
        if ($codeSha.Groups[1].Value -ne [string]$baseline.Value.sha512) {
            $problems.Add("'$baselineKey' sha512 differs between runtime_versions.json and ds_runtime_installer.gd.")
        }
        if ([long]$codeSize.Groups[1].Value -ne [long]$baseline.Value.size) {
            $problems.Add("'$baselineKey' size differs: manifest=$($baseline.Value.size) code=$($codeSize.Groups[1].Value)")
        }
    }
}

if ($problems.Count -gt 0) {
    Write-Host "[runtime-versions] FAILED: $($problems.Count) problem(s)." -ForegroundColor Red
    foreach ($problem in $problems) {
        Write-Host "  - $problem" -ForegroundColor Yellow
    }
    exit 1
}

$known = @($entries | ForEach-Object { $_.Name }) -join ', '
Write-Host "[runtime-versions] OK: $($entries.Count) package(s) [$known]; '$baselineKey' matches the built-in fallback." -ForegroundColor Green
exit 0
