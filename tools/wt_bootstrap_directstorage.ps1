<#
.SYNOPSIS
    Seed a worktree with the fetched DirectStorage SDK (import library + runtime DLLs).

.DESCRIPTION
    thirdparty/directstorage/lib/x64/dstorage.lib and thirdparty/directstorage/bin/x64/*.dll
    are Microsoft EULA binaries and are gitignored, so a fresh worktree has neither and
    SConstruct refuses to link. This pre-start hook copies them from the primary worktree
    instead of fetching again.

    It is deliberately not interactive: worktree creation must never show a license
    window or accept a license on the user's behalf. If the primary has not fetched the
    SDK, the hook fails and says to run the fetch task there.

    Copy (not hardlink) is intentional. The SDK is about 1.7 MB, while a hardlink would
    silently keep pointing at the old bytes when the primary re-fetches and replaces the
    files. The copy is also what the remote build tar carries to Device B.

.PARAMETER PrimaryWorktreePath
    The primary checkout that owns .cache/directstorage and the fetched SDK.

.PARAMETER WorktreePath
    Destination worktree. Defaults to the current directory, where the pre-start hook runs.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/wt_bootstrap_directstorage.ps1 <primary> <worktree>
#>
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification = 'Worktree pre-start hook; console output is the interface.')]
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PrimaryWorktreePath,
    [string]$WorktreePath = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PrimaryWorktreePath -PathType Container)) {
    Write-Host "ERROR: Primary worktree not found: $PrimaryWorktreePath" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $WorktreePath -PathType Container)) {
    Write-Host "ERROR: Destination worktree not found: $WorktreePath" -ForegroundColor Red
    exit 1
}

$primaryRoot = (Resolve-Path -LiteralPath $PrimaryWorktreePath).Path.TrimEnd('\', '/')
$worktreeRoot = (Resolve-Path -LiteralPath $WorktreePath).Path.TrimEnd('\', '/')

$sourceLib = Join-Path $primaryRoot 'thirdparty\directstorage\lib\x64\dstorage.lib'
if (-not (Test-Path -LiteralPath $sourceLib -PathType Leaf)) {
    Write-Host "ERROR: The primary worktree has no DirectStorage import library:" -ForegroundColor Red
    Write-Host "  $sourceLib" -ForegroundColor Red
    Write-Host "Fetch it in the primary first, then create the worktree:" -ForegroundColor Yellow
    Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1" -ForegroundColor Yellow
    Write-Host "  (or run the VS Code task 'Fetch DirectStorage Runtime')" -ForegroundColor Yellow
    exit 1
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        try {
            return [Convert]::ToBase64String($sha.ComputeHash($stream))
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha.Dispose()
    }
}

$pairs = @(
    [pscustomobject]@{
        Source = $sourceLib
        Destination = (Join-Path $worktreeRoot 'thirdparty\directstorage\lib\x64\dstorage.lib')
        Required = $true
    },
    [pscustomobject]@{
        Source = (Join-Path $primaryRoot 'thirdparty\directstorage\bin\x64\dstorage.dll')
        Destination = (Join-Path $worktreeRoot 'thirdparty\directstorage\bin\x64\dstorage.dll')
        Required = $false
    },
    [pscustomobject]@{
        Source = (Join-Path $primaryRoot 'thirdparty\directstorage\bin\x64\dstoragecore.dll')
        Destination = (Join-Path $worktreeRoot 'thirdparty\directstorage\bin\x64\dstoragecore.dll')
        Required = $false
    }
)

$copied = 0
foreach ($pair in $pairs) {
    if (-not (Test-Path -LiteralPath $pair.Source -PathType Leaf)) {
        if ($pair.Required) {
            Write-Host "ERROR: Missing required DirectStorage file: $($pair.Source)" -ForegroundColor Red
            exit 1
        }
        Write-Host "[wt] DirectStorage runtime DLL not present in the primary; skipping $($pair.Source)" -ForegroundColor DarkGray
        continue
    }

    $destinationDir = Split-Path -Path $pair.Destination -Parent
    if (-not (Test-Path -LiteralPath $destinationDir -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($destinationDir) | Out-Null
    }

    $sourceHash = Get-FileSha256 -Path $pair.Source
    if (Test-Path -LiteralPath $pair.Destination -PathType Leaf) {
        $destinationHash = Get-FileSha256 -Path $pair.Destination
        if ($destinationHash -eq $sourceHash) {
            Write-Host "[wt] DirectStorage file already current: $($pair.Destination)" -ForegroundColor DarkGray
            continue
        }
    }

    Copy-Item -LiteralPath $pair.Source -Destination $pair.Destination -Force
    $sourceLength = (Get-Item -LiteralPath $pair.Source).Length
    $destinationLength = (Get-Item -LiteralPath $pair.Destination).Length
    if ($sourceLength -ne $destinationLength) {
        Write-Host "ERROR: Copy verification failed for $($pair.Destination) (source $sourceLength bytes, destination $destinationLength bytes)" -ForegroundColor Red
        exit 1
    }
    Write-Host "[wt] Seeded $($pair.Destination) ($sourceLength bytes)"
    $copied++
}

Write-Host "[wt] DirectStorage SDK ready in the worktree ($copied file(s) copied)."
exit 0
