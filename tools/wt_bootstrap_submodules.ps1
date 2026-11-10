<#
.SYNOPSIS
    Initialize a new worktree's submodules from the primary worktree's local module stores.

.DESCRIPTION
    Invoked by the first pre-start hook in .config/wt.toml. The naive command,
    `git submodule update --init --recursive`, clones every submodule from its
    configured URL into .git/worktrees/<branch>/modules - a separate store per
    worktree, measured at 25-47 MB - so every worktree creation needs the network
    and, for a private submodule, a credential this shell happens to have.
    A missing credential looks like a dead remote: GitHub answers HTTP 404
    "repository not found" for a private repository instead of 401, so the failure
    reads like a wrong URL.

    The primary worktree already holds every pinned commit under .git/modules, so
    this script clones each submodule from there instead and leaves only the
    genuinely absent paths to the network. A local clone is also hardlinked, so it
    costs no extra disk.

    Nested submodules (extern/gdextest's extern/godot-cpp) are public and are
    completed by one recursive pass at the end. They are not seeded because their
    store lives inside the parent's module directory and the URL override would
    have to be threaded through the parent repository's config.

.PARAMETER PrimaryWorktreePath
    Absolute path of the primary worktree. The hook passes {{ primary_worktree_path }}.

.PARAMETER DestinationPath
    The new worktree root. Defaults to the current directory (the hook runs there).

.NOTES
    Exits non-zero so the pre-start hook aborts worktree creation instead of
    leaving a half-initialized tree behind - the failure that used to surface as
    an empty extern/gdextest and need `git submodule update --init --force` by hand.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PrimaryWorktreePath,
    [string]$DestinationPath = (Get-Location).Path
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingPath {
    param([string]$Path, [string]$What)
    try {
        return (Resolve-Path -LiteralPath $Path).Path.TrimEnd('\', '/')
    }
    catch {
        Write-Host "[wt] submodules: $What not found at '$Path'."
        exit 1
    }
}

$primary = Resolve-ExistingPath -Path $PrimaryWorktreePath -What "primary worktree"
$dest = Resolve-ExistingPath -Path $DestinationPath -What "worktree root"
$gitModulesPath = Join-Path $dest ".gitmodules"

if (-not (Test-Path -LiteralPath $gitModulesPath -PathType Leaf)) {
    Write-Host "[wt] submodules: no .gitmodules under '$dest'; nothing to initialize."
    exit 0
}

# `git config` prints "<key> <value>" per match and exits 1 when nothing matches.
$rawEntries = @(& git config -f $gitModulesPath --get-regexp '^submodule\..*\.path$' 2>&1)
if ($LASTEXITCODE -ne 0) {
    Write-Host "[wt] submodules: cannot read submodule paths from '$gitModulesPath' (git exit $LASTEXITCODE)."
    $rawEntries | ForEach-Object { Write-Host "  $_" }
    exit 1
}

function Invoke-GitStep {
    param([string[]]$Arguments, [string]$Label)
    # Under `2>&1` a native command's stderr arrives as an ErrorRecord, and the
    # 'Stop' preference set above for the path checks would turn git's ordinary
    # progress output ("Cloning into ...") into a terminating error. Capture it
    # instead and decide on the exit code.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& git @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -eq 0) { return $true }
    Write-Host "[wt] submodules: '$Label' failed (git exit $exitCode)." -ForegroundColor Red
    foreach ($line in $output) { Write-Host "  $line" }
    return $false
}

$primaryModules = Join-Path (Join-Path $primary ".git") "modules"
$seeded = @()
$nestedSeeded = @()
$fromNetwork = @()
$failed = @()
$localModuleByPath = @{}

foreach ($rawEntry in $rawEntries) {
    $entry = [string]$rawEntry
    if (-not $entry.Trim()) { continue }
    $splitAt = $entry.IndexOf(' ')
    if ($splitAt -lt 1) { continue }

    $key = $entry.Substring(0, $splitAt).Trim()
    $submodulePath = $entry.Substring($splitAt + 1).Trim()
    if ($key -notmatch '^submodule\.(.+)\.path$') { continue }
    $submoduleName = $Matches[1]

    $localModule = Join-Path $primaryModules $submodulePath
    $hasLocalModule = Test-Path -LiteralPath (Join-Path $localModule "HEAD")

    $updateArgs = @("submodule", "update", "--init")
    # A previous, aborted bootstrap can leave an empty directory behind; --force
    # lets git adopt it instead of refusing with "already exists".
    $submoduleDir = Join-Path $dest $submodulePath
    if ((Test-Path -LiteralPath $submoduleDir) -and -not (Test-Path -LiteralPath (Join-Path $submoduleDir ".git"))) {
        $updateArgs += "--force"
    }
    $updateArgs += @("--", $submodulePath)

    if ($hasLocalModule) {
        # protocol.file.allow: git >= 2.38 refuses local-path submodule clones by default.
        $stepArgs = @("-c", "protocol.file.allow=always", "-c", "submodule.$submoduleName.url=$localModule") + $updateArgs
        if (Invoke-GitStep -Arguments $stepArgs -Label "seed $submodulePath from the primary") {
            $seeded += $submodulePath
            $localModuleByPath[$submodulePath] = $localModule
        }
        else {
            $failed += $submodulePath
            Write-Host "[wt] submodules: 'repository not found' (HTTP 404) from GitHub means the remote is private and this shell has no credential for it - GitHub answers 404, not 401, so it reads like a wrong URL. The active account decides: check 'gh auth status'." -ForegroundColor Yellow
            Write-Host "[wt] submodules: the primary's copy is at '$localModule'; retry by hand with:" -ForegroundColor Yellow
            Write-Host "  git -c protocol.file.allow=always -c submodule.$submoduleName.url=$localModule submodule update --init --force $submodulePath" -ForegroundColor Yellow
        }
    }
    else {
        if (Invoke-GitStep -Arguments $updateArgs -Label "clone $submodulePath from the network") {
            $fromNetwork += $submodulePath
        }
        else {
            $failed += $submodulePath
            Write-Host "[wt] submodules: '$submodulePath' has no module store in the primary, so it needs the network and a credential that can read it." -ForegroundColor Yellow
        }
    }
}

# Nested submodules live inside the parent's module store, not at <primaryModules>/<path>:
# .git/modules/extern/gdextest/modules/extern/godot-cpp. Seed those from the primary too, so the
# recursive pass below needs no network at all.
foreach ($parentPath in @($localModuleByPath.Keys)) {
    $parentModule = [string]$localModuleByPath[$parentPath]
    $parentDir = Join-Path $dest $parentPath
    $nestedGitmodules = Join-Path $parentDir ".gitmodules"
    if (-not (Test-Path -LiteralPath $nestedGitmodules -PathType Leaf)) { continue }

    $nestedEntries = @(& git config -f $nestedGitmodules --get-regexp '^submodule\..*\.path$' 2>&1)
    if ($LASTEXITCODE -ne 0) { continue }

    foreach ($rawNested in $nestedEntries) {
        $nestedLine = [string]$rawNested
        $nestedSplit = $nestedLine.IndexOf(' ')
        if ($nestedSplit -lt 1) { continue }
        $nestedKey = $nestedLine.Substring(0, $nestedSplit).Trim()
        $nestedPath = $nestedLine.Substring($nestedSplit + 1).Trim()
        if ($nestedKey -notmatch '^submodule\.(.+)\.path$') { continue }
        $nestedName = $Matches[1]

        $nestedModule = Join-Path (Join-Path $parentModule "modules") $nestedPath
        # A module directory can exist but be an empty shell (HEAD with no objects). Only seed when
        # it actually contains the commit this parent pins; otherwise leave it to the recursive pass.
        $pinnedLine = @(& git -C $parentDir ls-files -s -- $nestedPath 2>&1)
        $pinnedCommit = ""
        if ($LASTEXITCODE -eq 0 -and $pinnedLine.Count -gt 0) {
            $pinnedParts = ([string]$pinnedLine[0]).Trim() -split '\s+'
            if ($pinnedParts.Count -ge 2) { $pinnedCommit = $pinnedParts[1] }
        }
        if (-not $pinnedCommit) { continue }
        & git -C $nestedModule cat-file -e "$pinnedCommit^{commit}" 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            # Not available locally; leave it to the recursive pass below.
            continue
        }

        $nestedArgs = @("-C", $parentDir, "-c", "protocol.file.allow=always", "-c", "submodule.$nestedName.url=$nestedModule", "submodule", "update", "--init")
        $nestedDir = Join-Path $parentDir $nestedPath
        if ((Test-Path -LiteralPath $nestedDir) -and -not (Test-Path -LiteralPath (Join-Path $nestedDir ".git"))) {
            $nestedArgs += "--force"
        }
        $nestedArgs += @("--", $nestedPath)

        if (Invoke-GitStep -Arguments $nestedArgs -Label "seed nested $parentPath/$nestedPath from the primary") {
            $nestedSeeded += "$parentPath/$nestedPath"
        }
        else {
            $failed += "$parentPath/$nestedPath"
            Write-Host "[wt] submodules: the primary's nested copy is at '$nestedModule'; retry by hand with:" -ForegroundColor Yellow
            Write-Host "  git -C $parentDir -c protocol.file.allow=always -c submodule.$nestedName.url=$nestedModule submodule update --init --force $nestedPath" -ForegroundColor Yellow
        }
    }
}

# Anything still missing falls back to the network: the parent is initialized now, so one
# recursive pass completes it.
if ($failed.Count -eq 0) {
    if (-not (Invoke-GitStep -Arguments @("submodule", "update", "--init", "--recursive") -Label "recursive submodule update (nested)")) {
        $failed += "(nested)"
    }
}

function Format-PathList {
    param([string[]]$Paths)
    if ($Paths.Count -eq 0) { return "none" }
    return ($Paths -join ", ")
}

Write-Host ("[wt] submodules: seeded from the primary: {0}" -f (Format-PathList $seeded))
Write-Host ("[wt] submodules: seeded nested: {0}" -f (Format-PathList $nestedSeeded))
Write-Host ("[wt] submodules: from the network: {0}" -f (Format-PathList $fromNetwork))

if ($failed.Count -gt 0) {
    Write-Host ("[wt] submodules: FAILED: {0}" -f (Format-PathList $failed)) -ForegroundColor Red
    exit 1
}

exit 0
