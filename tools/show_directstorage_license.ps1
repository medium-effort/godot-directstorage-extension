<#
.SYNOPSIS
    Show the Microsoft DirectStorage package license and record an accept/decline decision.

.DESCRIPTION
    Launched by tools/fetch_directstorage.ps1 in a separate PowerShell console so the
    fetch log stays in the VS Code task terminal. The full LICENSE.txt extracted from
    the verified NuGet package is paged here, and the answer is written to
    -DecisionPath before this process exits.

    The parent treats a missing or unreadable decision file as a decline, so closing
    this window can never be mistaken for license acceptance (even though the process
    then exits with code 0).

    -Decision Accept|Decline is the non-interactive test/automation seam: it records
    the answer without displaying the license.

.PARAMETER LicensePath
    LICENSE.txt extracted from the verified package.

.PARAMETER Version
    Package version being installed; printed in the header only.

.PARAMETER DecisionPath
    File that receives exactly "accept" or "decline". The parent reads this file, not
    the exit code.

.PARAMETER Decision
    Skip the prompt and record this answer. Used by tests and by the fetch script's
    explicit -Decision switch.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/show_directstorage_license.ps1 -LicensePath .cache/directstorage/license-1.3.0.txt -Version 1.3.0 -DecisionPath .cache/directstorage/decision-1.3.0.txt
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Interactive license viewer; Write-Host is the intended console output.')]
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$LicensePath,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$DecisionPath,
    [ValidateSet('', 'Accept', 'Decline')][string]$Decision = ''
)

$ErrorActionPreference = 'Stop'

function Write-Decision {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $parent = Split-Path -Path $Path -Parent
    if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Value, [System.Text.UTF8Encoding]::new($false))
}

if (-not (Test-Path -LiteralPath $LicensePath -PathType Leaf)) {
    Write-Host "ERROR: License file not found: $LicensePath" -ForegroundColor Red
    Write-Host "Nothing was installed." -ForegroundColor Red
    exit 2
}

# Automation/test seam: never prompt, just record the requested answer.
if (-not [string]::IsNullOrWhiteSpace($Decision)) {
    if ($Decision -eq 'Accept') {
        Write-Decision -Path $DecisionPath -Value 'accept'
        Write-Host "DirectStorage $Version license recorded as accepted." -ForegroundColor Green
        exit 0
    }
    Write-Decision -Path $DecisionPath -Value 'decline'
    Write-Host "DirectStorage $Version license recorded as declined." -ForegroundColor Yellow
    exit 1
}

Write-Host ''
Write-Host "Microsoft DirectStorage SDK $Version" -ForegroundColor Cyan
Write-Host 'Read the license below, then type ACCEPT to install or DECLINE to cancel.' -ForegroundColor Gray
Write-Host 'Nothing is extracted into the repository unless you accept.' -ForegroundColor DarkGray
Write-Host ''

try {
    Get-Content -LiteralPath $LicensePath -Encoding UTF8 | Out-Host -Paging
}
catch {
    # Some hosts do not support paging; print the license rather than lose it.
    Get-Content -LiteralPath $LicensePath -Encoding UTF8 | Out-Host
}

while ($true) {
    $answer = Read-Host 'Type ACCEPT or DECLINE'
    if ($null -eq $answer) {
        # Console closed or input unavailable: record nothing, and the parent's
        # missing-decision check turns this into a decline.
        Write-Host 'No answer recorded; treating as declined.' -ForegroundColor Yellow
        exit 1
    }

    switch ($answer.Trim().ToUpperInvariant()) {
        'ACCEPT' {
            Write-Decision -Path $DecisionPath -Value 'accept'
            Write-Host 'License accepted. You can close this window.' -ForegroundColor Green
            exit 0
        }
        'DECLINE' {
            Write-Decision -Path $DecisionPath -Value 'decline'
            Write-Host 'License declined; nothing was installed.' -ForegroundColor Yellow
            exit 1
        }
        default {
            Write-Host 'Please type ACCEPT or DECLINE.' -ForegroundColor Yellow
        }
    }
}
