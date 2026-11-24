<#
.SYNOPSIS
    Seed a worktree with the pinned GUT archive from the primary's download cache.

.DESCRIPTION
    project/addons/gut/ and .cache/gut/ are gitignored, so a fresh worktree has neither.
    tools/run_gut.py fetches GUT on demand, which means a network download in every
    worktree and a hard failure in an offline session. This pre-start hook copies only
    the verified gut-v*.zip archives from the primary's .cache/gut; run_gut.py then
    SHA-verifies and extracts locally into that worktree's project/addons/gut.

    Copy (not hardlink) is intentional. The archive is about 1.1 MB, and a hardlink would
    keep pointing at the primary's bytes if the archive is ever replaced.

    It is deliberately not interactive and never downloads: worktree creation must not
    open a network fetch on the user's behalf. If the primary has no cached archive, the
    hook fails and says to run tools/fetch_gut.py there.

    Resolved from the primary, not the new worktree: this hook has no branch-specific
    logic (run_gut.py reads the pin), and a worktree created from an older commit would
    otherwise fail because this script does not exist there yet.

.PARAMETER PrimaryWorktreePath
    The primary checkout that owns .cache/gut.

.PARAMETER WorktreePath
    Destination worktree. Defaults to the current directory, where the pre-start hook runs.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/wt_bootstrap_gut.ps1 <primary> <worktree>
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

$sourceCache = Join-Path $primaryRoot '.cache\gut'
$destinationCache = Join-Path $worktreeRoot '.cache\gut'

if (-not (Test-Path -LiteralPath $sourceCache -PathType Container)) {
    Write-Host "ERROR: The primary worktree has no GUT download cache: $sourceCache" -ForegroundColor Red
    Write-Host "Fetch the pinned GUT in the primary first, then create the worktree:" -ForegroundColor Yellow
    Write-Host "  python tools/fetch_gut.py" -ForegroundColor Yellow
    exit 1
}

$archives = @(Get-ChildItem -LiteralPath $sourceCache -Filter 'gut-v*.zip' -File)
if ($archives.Count -eq 0) {
    Write-Host "ERROR: No gut-v*.zip archive in $sourceCache" -ForegroundColor Red
    Write-Host "Fetch the pinned GUT in the primary first, then create the worktree:" -ForegroundColor Yellow
    Write-Host "  python tools/fetch_gut.py" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path -LiteralPath $destinationCache -PathType Container)) {
    [System.IO.Directory]::CreateDirectory($destinationCache) | Out-Null
}

# Use .NET directly, not Get-FileHash: worktrunk invokes hooks in a nested
# powershell.exe where cmdlet autoload can fail with CommandNotFoundException.
# The DirectStorage bootstrap hook uses the same helper for the same reason.
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

$copied = 0
foreach ($archive in $archives) {
    $destination = Join-Path $destinationCache $archive.Name
    $sourceHash = Get-FileSha256 -Path $archive.FullName
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $destinationHash = Get-FileSha256 -Path $destination
        if ($destinationHash -eq $sourceHash) {
            Write-Host "[wt] GUT archive already current: $($archive.Name)" -ForegroundColor DarkGray
            continue
        }
    }

    Copy-Item -LiteralPath $archive.FullName -Destination $destination -Force
    if ((Get-Item -LiteralPath $destination).Length -ne $archive.Length) {
        Write-Host "ERROR: Copy verification failed for $destination" -ForegroundColor Red
        exit 1
    }
    Write-Host "[wt] Seeded GUT archive $($archive.Name) ($($archive.Length) bytes)"
    $copied++
}

Write-Host "[wt] GUT cache ready in the worktree ($copied archive(s) copied; run_gut.py extracts locally)."
exit 0
