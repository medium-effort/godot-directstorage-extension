<#
.SYNOPSIS
    Report where a worktree branch started and whether its work is already in the target.

.DESCRIPTION
    Answers the question a worktree agent asks before landing, in one screen:
      - which worktree and branch am I in, and where is the primary?
      - which branch was this worktree created from (git's branch reflog records it)?
      - what is the fork-point commit, even if the base branch has since moved or been rewritten?
      - how far ahead/behind the target am I, and is my work already in it (git cherry)?
      - what is the exact command to replay only my commits onto a rewritten target?

    The fork point is the value a plain `git rebase <target>` gets wrong after a squash or reset:
    without it, the replay includes commits from the old target that are no longer ancestors.
    See .agents/skills/do-it-in-a-worktree/references/rewritten-target.md.

.PARAMETER Branch
    Branch to inspect. Defaults to the branch checked out in the current directory.

.PARAMETER Target
    Branch the work is meant to land on. Defaults to origin's default branch, then `main`.

.PARAMETER RepositoryPath
    Any path inside the repository. Defaults to the current directory.

.NOTES
    Read-only: it never moves a ref or touches a working tree.
#>
[CmdletBinding()]
param(
    [string]$Branch = "",
    [string]$Target = "",
    [string]$RepositoryPath = ""
)

function Invoke-Git {
    param([string[]]$Arguments)
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& git -C $script:RepoRoot @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

if ([string]::IsNullOrWhiteSpace($RepositoryPath)) { $RepositoryPath = (Get-Location).Path }
if (-not (Test-Path -LiteralPath $RepositoryPath -PathType Container)) {
    Write-Host "ERROR: not a directory: $RepositoryPath" -ForegroundColor Red
    exit 1
}

# Invoke-Git runs against $script:RepoRoot; start it at the caller's path so the
# very first call (rev-parse --show-toplevel) has a working directory.
$script:RepoRoot = $RepositoryPath

$topLevel = Invoke-Git -Arguments @("rev-parse", "--show-toplevel")
if ($topLevel.ExitCode -ne 0 -or -not $topLevel.Output) {
    Write-Host "ERROR: not inside a git repository: $RepositoryPath" -ForegroundColor Red
    exit 1
}
$script:RepoRoot = ([string]$topLevel.Output[0]).Trim()

if ([string]::IsNullOrWhiteSpace($Branch)) {
    $head = Invoke-Git -Arguments @("rev-parse", "--abbrev-ref", "HEAD")
    if ($head.ExitCode -ne 0) {
        Write-Host "ERROR: cannot resolve HEAD in $script:RepoRoot" -ForegroundColor Red
        exit 1
    }
    $Branch = ([string]$head.Output[0]).Trim()
}
if ($Branch -eq "HEAD") {
    Write-Host "ERROR: HEAD is detached here; pass -Branch <name>." -ForegroundColor Red
    exit 1
}

if ([string]::IsNullOrWhiteSpace($Target)) {
    $originHead = Invoke-Git -Arguments @("symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD")
    if ($originHead.ExitCode -eq 0 -and $originHead.Output) {
        $Target = ([string]$originHead.Output[0]).Trim() -replace '^origin/', ''
    }
    elseif ((Invoke-Git -Arguments @("rev-parse", "--verify", "--quiet", "refs/heads/main")).ExitCode -eq 0) {
        $Target = "main"
    }
    else {
        Write-Host "ERROR: cannot infer the target branch; pass -Target <name>." -ForegroundColor Red
        exit 1
    }
}

# Worktree list: the first block is the primary, and the block naming $Branch is the checkout.
$worktreePath = ""
$primaryPath = ""
$blocks = @()
$current = @{}
foreach ($line in (Invoke-Git -Arguments @("worktree", "list", "--porcelain")).Output) {
    $text = [string]$line
    if ([string]::IsNullOrWhiteSpace($text)) {
        if ($current.Count -gt 0) { $blocks += [pscustomobject]$current; $current = @{} }
        continue
    }
    if ($text -match '^worktree (.+)$') { $current["path"] = $Matches[1].Trim(); continue }
    if ($text -match '^branch refs/heads/(.+)$') { $current["branch"] = $Matches[1].Trim() }
}
if ($current.Count -gt 0) { $blocks += [pscustomobject]$current }
if ($blocks.Count -gt 0) { $primaryPath = [string]$blocks[0].path }
foreach ($block in $blocks) {
    if ($block.PSObject.Properties.Name -contains 'branch' -and [string]$block.branch -eq $Branch) {
        $worktreePath = [string]$block.path
        break
    }
}

# Creation base: git writes "branch: Created from <start-point>" as the oldest reflog entry.
$createdFrom = ""
$reflog = Invoke-Git -Arguments @("reflog", "show", "--format=%gs", $Branch)
if ($reflog.ExitCode -eq 0 -and $reflog.Output.Count -gt 0) {
    $oldest = [string]$reflog.Output[$reflog.Output.Count - 1]
    if ($oldest -match '^branch: Created from (.+)$') { $createdFrom = $Matches[1].Trim() }
}

# Fork point: prefer --fork-point (it reads the base branch's reflog, so it survives the base moving).
$forkPoint = ""
if ($createdFrom -and $createdFrom -ne "HEAD") {
    $fork = Invoke-Git -Arguments @("merge-base", "--fork-point", $createdFrom, $Branch)
    if ($fork.ExitCode -eq 0 -and $fork.Output.Count -gt 0) { $forkPoint = ([string]$fork.Output[0]).Trim() }
}
if (-not $forkPoint) {
    $fork = Invoke-Git -Arguments @("merge-base", $Target, $Branch)
    if ($fork.ExitCode -eq 0 -and $fork.Output.Count -gt 0) { $forkPoint = ([string]$fork.Output[0]).Trim() }
}

$counts = Invoke-Git -Arguments @("rev-list", "--left-right", "--count", "$Target...$Branch")
$ahead = "?"
$behind = "?"
if ($counts.ExitCode -eq 0 -and $counts.Output.Count -gt 0) {
    $parts = ([string]$counts.Output[0]).Trim() -split '\s+'
    if ($parts.Count -ge 2) { $behind = $parts[0]; $ahead = $parts[1] }
}

$isAncestor = (Invoke-Git -Arguments @("merge-base", "--is-ancestor", $Branch, $Target)).ExitCode -eq 0

$cherry = Invoke-Git -Arguments @("cherry", $Target, $Branch)
$appliedCount = 0
$pendingCount = 0
if ($cherry.ExitCode -eq 0) {
    foreach ($line in $cherry.Output) {
        $text = [string]$line
        if ($text.StartsWith("- ")) { $appliedCount++ }
        elseif ($text.StartsWith("+ ")) { $pendingCount++ }
    }
}

Write-Host ""
Write-Host "branch   : $Branch"
Write-Host "worktree : $(if ($worktreePath) { $worktreePath } else { '(not checked out)' })"
Write-Host "primary  : $primaryPath"
Write-Host "target   : $Target"
Write-Host "created  : $(if ($createdFrom) { "from $createdFrom (branch reflog)" } else { '(no creation entry in the reflog)' })"
Write-Host "fork     : $(if ($forkPoint) { $forkPoint } else { '(unknown)' })"
Write-Host "position : ahead $ahead, behind $behind (vs $Target)"
Write-Host "ancestor : $(if ($isAncestor) { 'yes - nothing to land' } else { 'no - this branch has commits to land' })"
Write-Host "cherry   : $appliedCount already in $Target, $pendingCount not applied"

if ($isAncestor) {
    Write-Host "next     : nothing to land; remove the worktree (see references/worktree-removal.md)"
}
elseif ($forkPoint) {
    Write-Host "next     : if $Target was rewritten (squash/reset/amend), use:"
    Write-Host "           git rebase --onto $Target $forkPoint $Branch"
    Write-Host "           then verify: git log --oneline $Target..$Branch; git cherry $Target $Branch"
}
else {
    Write-Host "next     : fork point unknown; find the last shared commit with: git merge-base $Target $Branch"
}

exit 0
