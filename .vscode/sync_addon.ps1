# Syncs build output into the external project(s) without building.
#
# The implementation lives in tools/sync_artifacts.py (shared with
# build_remote.ps1 step [6/6]); this wrapper resolves the configuration and the
# source checkout, then forwards arguments. Use it when only GDScript or
# non-C++ addon files changed and the C++ DLLs are already up to date.
#
#   .\sync_addon.ps1                        -> current checkout, SYNC_ADDON_DIR
#   .\sync_addon.ps1 -Source <worktree>     -> publish that worktree's addon
#   .\sync_addon.ps1 -AddonDir <path>       -> override SYNC_ADDON_DIR for this run
#   .\sync_addon.ps1 -ListSources           -> show the sources you can pick
#   .\sync_addon.ps1 -Targets all           -> test, copyable and addon targets
#   .\sync_addon.ps1 -Targets copyable      -> SYNC_ADDON_COPYABLE_DIR only
#   .\sync_addon.ps1 -DryRun                -> report what would change
#
# -Source accepts a worktree name (runtime-manager-preview-toggle), that
# worktree's branch name, or its path; an empty value keeps the current
# checkout. Only worktrees of this repository are accepted: a worktree has its
# own build, so its addon is published as it stands (GDScript and DLLs
# together). The SYNC_* destinations always come from this checkout's .env -
# another worktree's .env never decides where anything is written.
param(
    [switch]$Verbose,
    [string]$Targets = "addon",
    [string]$Source = "",
    [string]$AddonDir = "",
    [switch]$DryRun,
    [switch]$ListSources
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$EnvFilePath = Join-Path $RepoRoot ".env"
$SyncScript = Join-Path $RepoRoot "tools\sync_artifacts.py"
$AddonRelPath = Join-Path "project" (Join-Path "addons" "directstorage_accelerator")

function Get-RepoWorktreeList {
    # `git worktree list --porcelain` prints one block per checkout: a
    # "worktree <path>" line plus "branch refs/heads/<name>" or "detached".
    $worktrees = @()
    $block = @{}
    foreach ($line in @(& git -C $RepoRoot worktree list --porcelain 2>$null)) {
        $text = [string]$line
        if ([string]::IsNullOrWhiteSpace($text)) {
            if ($block.Count -gt 0) { $worktrees += [pscustomobject]$block; $block = @{} }
            continue
        }
        if ($text -match '^worktree (.+)$') { $block["Path"] = $Matches[1].Trim(); continue }
        if ($text -match '^branch refs/heads/(.+)$') { $block["Branch"] = $Matches[1].Trim(); continue }
        if ($text -eq 'detached') { $block["Branch"] = "(detached)" }
    }
    if ($block.Count -gt 0) { $worktrees += [pscustomobject]$block }
    return $worktrees
}

function Format-WorktreeChoice {
    param($Worktree)
    $name = Split-Path -Leaf ([string]$Worktree.Path)
    $branch = if ($Worktree.PSObject.Properties.Name -contains 'Branch') { [string]$Worktree.Branch } else { '(unknown)' }
    $isCurrent = ([string]$Worktree.Path).TrimEnd('\', '/') -ieq $RepoRoot.TrimEnd('\', '/')
    $suffix = if ($isCurrent) { "  <- current checkout" } else { "" }
    return "{0}  [{1}]{2}" -f $name, $branch, $suffix
}

$Worktrees = @(Get-RepoWorktreeList)

$CurrentBranch = ""
$currentHead = @(& git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null)
if ($currentHead.Count -gt 0) { $CurrentBranch = ([string]$currentHead[0]).Trim() }
if ($CurrentBranch -eq "HEAD") { $CurrentBranch = "" }

if ($ListSources) {
    Write-Host "Sync sources (pass one to -Source):" -ForegroundColor Cyan
    foreach ($worktree in $Worktrees) {
        Write-Host ("  {0}" -f (Format-WorktreeChoice $worktree))
        Write-Host ("    {0}" -f $worktree.Path) -ForegroundColor DarkGray
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $EnvFilePath)) {
    Write-Host "Configuration file '.env' not found! Please copy '.env.example' to '.env' and configure SYNC_ADDON_DIR." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $SyncScript)) {
    Write-Host "Sync implementation not found: $SyncScript" -ForegroundColor Red
    exit 1
}

$SourceRoot = $RepoRoot
$SourceLabel = if ($CurrentBranch) { "current checkout ($CurrentBranch)" } else { "current checkout" }

if (-not [string]::IsNullOrWhiteSpace($Source)) {
    $needle = $Source.Trim()
    $resolved = $null

    if ($needle -notin @(".", "current", "HEAD", "@")) {
        # Path first (absolute or relative to the repository root), then the
        # worktree directory name, then the branch it has checked out.
        $candidate = $needle
        if (-not [System.IO.Path]::IsPathRooted($candidate)) { $candidate = Join-Path $RepoRoot $candidate }
        $candidatePath = $null
        try { $candidatePath = (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).Path } catch { $candidatePath = $null }

        foreach ($worktree in $Worktrees) {
            if ($candidatePath -and ([string]$worktree.Path).TrimEnd('\', '/') -ieq $candidatePath.TrimEnd('\', '/')) { $resolved = $worktree; break }
        }
        if (-not $resolved) {
            foreach ($worktree in $Worktrees) {
                if ((Split-Path -Leaf ([string]$worktree.Path)) -ieq $needle) { $resolved = $worktree; break }
            }
        }
        if (-not $resolved) {
            foreach ($worktree in $Worktrees) {
                if ($worktree.PSObject.Properties.Name -contains 'Branch' -and [string]$worktree.Branch -ieq $needle) { $resolved = $worktree; break }
            }
        }
        if (-not $resolved) {
            Write-Host "Unknown sync source '$needle'." -ForegroundColor Red
            Write-Host "Available sources:" -ForegroundColor Yellow
            foreach ($worktree in $Worktrees) { Write-Host ("  {0}" -f (Format-WorktreeChoice $worktree)) }
            Write-Host "Pass a worktree name, its branch, or its path; omit -Source for the current checkout." -ForegroundColor Yellow
            exit 2
        }
    }

    if ($resolved) {
        $SourceRoot = (Resolve-Path -LiteralPath ([string]$resolved.Path) -ErrorAction Stop).Path
        $SourceLabel = if ($resolved.PSObject.Properties.Name -contains 'Branch') { [string]$resolved.Branch } else { Split-Path -Leaf $SourceRoot }
    }
}

$SourceAddon = Join-Path $SourceRoot $AddonRelPath
if (-not (Test-Path -LiteralPath $SourceAddon -PathType Container)) {
    Write-Host "Addon directory not found in sync source '$SourceLabel': $SourceAddon" -ForegroundColor Red
    exit 1
}

# Always name the resolved source: the same task run from a worktree window means
# a different checkout (and a different .env) than from the primary.
Write-Host "Sync source: $SourceLabel" -ForegroundColor Cyan
Write-Host "             $SourceRoot" -ForegroundColor DarkGray

$IsCurrentCheckout = $SourceRoot.TrimEnd('\', '/') -ieq $RepoRoot.TrimEnd('\', '/')
if (-not $IsCurrentCheckout) {
    # The extension DLLs are gitignored: they exist in a worktree only after a
    # build there. Publishing a worktree without them would pair one branch's
    # GDScript with another build's binaries.
    $binDir = Join-Path $SourceAddon "bin"
    $builtDlls = @(Get-ChildItem -LiteralPath $binDir -Filter "dsgd*.dll" -File -ErrorAction SilentlyContinue)
    if ($builtDlls.Count -eq 0) {
        $builtDlls = @(Get-ChildItem -LiteralPath $binDir -Filter "libdsgd*.dll" -File -ErrorAction SilentlyContinue)
    }
    if ($builtDlls.Count -eq 0) {
        Write-Host "Sync source '$SourceLabel' has no built extension DLL:" -ForegroundColor Red
        Write-Host "  $binDir" -ForegroundColor Red
        Write-Host "Build it there first (build_remote.ps1 in that worktree); the binaries are gitignored, so a fresh checkout never has them." -ForegroundColor Yellow
        exit 1
    }
}

$SyncArgs = @($SyncScript, "--targets", $Targets)
if ($Verbose) { $SyncArgs += "--verbose" }
if ($DryRun) { $SyncArgs += "--dry-run" }
if (-not [string]::IsNullOrWhiteSpace($AddonDir)) { $SyncArgs += @("--addon-dir", $AddonDir.Trim()) }
if (-not $IsCurrentCheckout) { $SyncArgs += @("--source-root", $SourceRoot, "--source-label", $SourceLabel) }

& python @SyncArgs
exit $LASTEXITCODE
