<#
.SYNOPSIS
    Share one prebuilt godot-cpp with a worktree instead of recompiling it there.

.DESCRIPTION
    godot-cpp is ~1,000 generated class translation units plus a large static
    library, and it is identical in every worktree of the same commit. SCons'
    object cache cannot remove that per-worktree cost: a cache hit still
    materialises a real .obj/.lib inside the tree (the linker needs a file there,
    and SCons never hardlinks). Measured cost of a built worktree: ~1,040 MB,
    of which ~424 MB is godot-cpp objects and ~436 MB is the dev static library.

    This script gives a worktree those two pieces for ~0 extra bytes:

      * godot-cpp/gen  -> junction to <primary>/.cache/godot-cpp-shared/gen
        (generated sources with objects filtered out, ~16 MB, shared by all
        worktrees; read-only in practice because build_library=no means nothing
        compiles inside it)
      * godot-cpp/bin/*.lib -> hardlinks to the same store, one per variant the
        primary has built. Hardlinks cost no disk and are real files for the
        linker, and godot-cpp appends its library to LIBS even with
        build_library=no, so no SConstruct surgery is needed.

    Pairs with GODOT_CPP_PREBUILT=1 in the worktree .env (written by
    tools/wt_bootstrap_env.ps1), which forces build_library=no and fails loudly
    if the library or gen/include is missing.

    Preconditions, all verified here or in SConstruct:
      * the primary worktree has built godot-cpp at least once (godot-cpp/gen
        exists), and its bin contains the library for the target you build;
      * same godot-cpp submodule commit (stamped, refreshed automatically);
      * same volume (junction/hardlink requirement on Windows).

.NOTES
    Safe to re-run. Real directories and real files already present in the
    worktree are left alone - this only ever adds links where nothing exists -
    with one exception: a library link that no longer matches the primary's
    bytes is refreshed (see the libraries section). That makes a re-run the
    repair step after the primary relinks godot-cpp (issue #0216).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PrimaryWorktreePath,
    [Parameter(Mandatory = $true)][string]$WorktreePath,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$primary = (Resolve-Path -LiteralPath $PrimaryWorktreePath).Path.TrimEnd('\', '/')
$worktree = (Resolve-Path -LiteralPath $WorktreePath).Path.TrimEnd('\', '/')
$primaryGen = Join-Path $primary "godot-cpp\gen"
$primaryBin = Join-Path $primary "godot-cpp\bin"
$store = Join-Path $primary ".cache\godot-cpp-shared"

if (-not (Test-Path -LiteralPath (Join-Path $primaryGen "include") -PathType Container)) {
    Write-Host "[wt] primary godot-cpp/gen is not generated yet - skipping prebuilt sharing (this worktree will build godot-cpp itself)."
    exit 0
}

# The worktree takes its SConstruct from its own branch, which may predate
# GODOT_CPP_PREBUILT support. Without that support the build would compile
# godot-cpp THROUGH this junction and relink the hardlinked library in place,
# corrupting the shared store for every other worktree. Refuse to link anything
# unless the tree on disk knows how to use it; the worktree then simply builds
# godot-cpp itself (~1 GB, slower, but safe).
$worktreeSconstruct = Join-Path $worktree "SConstruct"
$supportsPrebuilt = $false
if (Test-Path -LiteralPath $worktreeSconstruct -PathType Leaf) {
    $supportsPrebuilt = [bool](Select-String -LiteralPath $worktreeSconstruct -Pattern 'GODOT_CPP_PREBUILT' -Quiet)
}
if (-not $supportsPrebuilt) {
    Write-Host "[wt] this worktree's SConstruct has no GODOT_CPP_PREBUILT support (older commit, or the setup is not committed yet) - skipping sharing so godot-cpp is built locally instead of corrupting the shared store."
    exit 0
}

# One generated tree per godot-cpp commit, in its own directory. Never refresh in
# place: other worktrees are junctioned to an existing tree and may be building
# against it right now, and deleting or rewriting those files is what produced
# "used by another process" mid-build. A new commit simply gets a new directory;
# old ones stay until someone removes them.
$commit = ""
try {
    $commit = (& git -C (Join-Path $primary "godot-cpp") rev-parse HEAD 2>$null | Out-String).Trim()
}
catch {
    $commit = "unknown"
}
$commitTag = if ($commit -match '^[0-9a-f]{7,}$') { $commit.Substring(0, 12) } else { "unknown" }
$storeGen = Join-Path $store "gen-$commitTag"
$stampFile = Join-Path $storeGen ".stamp"

if (-not (Test-Path -LiteralPath (Join-Path $storeGen "include") -PathType Container)) {
    New-Item -ItemType Directory -Force -Path (Join-Path $storeGen "include") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $storeGen "src") | Out-Null
    # /XF *.obj: never share object files, they are per-target and per-tree.
    & robocopy (Join-Path $primaryGen "include") (Join-Path $storeGen "include") /E /XF *.obj /NFL /NDL /NJH /NJS /NP | Out-Null
    & robocopy (Join-Path $primaryGen "src") (Join-Path $storeGen "src") /E /XF *.obj /NFL /NDL /NJH /NJS /NP | Out-Null
    Set-Content -LiteralPath $stampFile -Value $commit -Encoding utf8
    Write-Host "[wt] created shared generated sources for godot-cpp commit $commitTag"
}
elseif ($Force) {
    Write-Host "[wt] -Force ignored: recreating a shared tree can break worktrees building against it. Remove $storeGen manually while nothing is building, then re-run."
}

# Worktree: gen as a junction (skip only when the worktree already has its own
# generated tree, e.g. it was built before sharing was enabled).
$worktreeGen = Join-Path $worktree "godot-cpp\gen"
if (Test-Path -LiteralPath $worktreeGen) {
    $genItem = Get-Item -LiteralPath $worktreeGen -Force
    if ($genItem.LinkType -eq "Junction") {
        $currentTarget = @($genItem.Target)[0]
        if ($currentTarget -and $currentTarget.TrimEnd('\', '/') -ne $storeGen.TrimEnd('\', '/')) {
            Write-Host "[wt] this worktree's godot-cpp/gen still points at an older shared tree ($currentTarget); it keeps working. Re-run this after the worktree is idle to switch it to commit $commitTag."
        }
    }
    else {
        Write-Host "[wt] worktree already has a real godot-cpp/gen; leaving it alone."
    }
}
else {
    New-Item -ItemType Junction -Path $worktreeGen -Target $storeGen | Out-Null
    Write-Host "[wt] linked godot-cpp/gen -> shared generated sources ($commitTag)"
}

# Libraries: hardlink every variant the primary has built, so whichever
# target/dev_build the worktree asks for is present, and refresh any link that
# no longer carries the primary's bytes.
#
# The invariant is "the store mirrors the primary": the primary owns these
# artifacts. SCons rebuilds a static library as a NEW file (unlink + create),
# which silently severs an existing hardlink, so a relink in the primary leaves
# the store - and every worktree linked through it - serving the previous bytes
# (issue #0216). Nothing detects that on its own, so a re-run of this script is
# the repair: a stale store entry is replaced with a fresh link to the primary's
# file, then any stale worktree entry is repointed at the store. Replacing a
# name never destroys data while another name still references the inode, so a
# refresh that fails halfway loses nothing.
$worktreeBin = Join-Path $worktree "godot-cpp\bin"
New-Item -ItemType Directory -Force -Path $worktreeBin | Out-Null
New-Item -ItemType Directory -Force -Path $store | Out-Null
$linked = 0
$refreshed = 0
$refreshFailures = 0

function Test-SameFile {
    param([string]$Left, [string]$Right)
    # Two names for one inode report the same size and the same write time; a
    # relink in the primary rewrites both. Cheaper and more robust than hashing
    # a 400+ MB library on every run.
    $l = Get-Item -LiteralPath $Left -Force -ErrorAction SilentlyContinue
    $r = Get-Item -LiteralPath $Right -Force -ErrorAction SilentlyContinue
    if (-not $l -or -not $r) { return $false }
    return ($l.Length -eq $r.Length) -and ($l.LastWriteTimeUtc -eq $r.LastWriteTimeUtc)
}

function Update-HardLink {
    param([string]$LinkPath, [string]$TargetPath)
    try {
        if (Test-Path -LiteralPath $LinkPath) {
            Remove-Item -LiteralPath $LinkPath -Force -ErrorAction Stop
        }
        New-Item -ItemType HardLink -Path $LinkPath -Target $TargetPath -ErrorAction Stop | Out-Null
        return $true
    }
    catch {
        # A build holding the file open is the expected cause; the old bytes stay
        # reachable through whichever names still exist.
        Write-Host ("[wt] could not refresh '" + $LinkPath + "' (a build may be using it): " + $_.Exception.Message)
        return $false
    }
}

if (Test-Path -LiteralPath $primaryBin -PathType Container) {
    foreach ($lib in @(Get-ChildItem -LiteralPath $primaryBin -Filter *.lib -File -ErrorAction SilentlyContinue)) {
        $storeLib = Join-Path $store $lib.Name
        if (-not (Test-Path -LiteralPath $storeLib)) {
            New-Item -ItemType HardLink -Path $storeLib -Target $lib.FullName | Out-Null
        }
        elseif (-not (Test-SameFile -Left $storeLib -Right $lib.FullName)) {
            if (Update-HardLink -LinkPath $storeLib -TargetPath $lib.FullName) {
                $refreshed++
                Write-Host "[wt] refreshed shared store entry '$($lib.Name)' from the primary"
            }
            else {
                $refreshFailures++
            }
        }
        $worktreeLib = Join-Path $worktreeBin $lib.Name
        if (-not (Test-Path -LiteralPath $worktreeLib)) {
            New-Item -ItemType HardLink -Path $worktreeLib -Target $storeLib | Out-Null
            $linked++
        }
        elseif (-not (Test-SameFile -Left $worktreeLib -Right $storeLib)) {
            if (Update-HardLink -LinkPath $worktreeLib -TargetPath $storeLib) {
                $refreshed++
                Write-Host "[wt] refreshed worktree link '$($lib.Name)' to the shared store"
            }
            else {
                $refreshFailures++
            }
        }
    }
}
Write-Host "[wt] hardlinked $linked godot-cpp librar(y|ies) into the worktree (0 extra bytes)"
if ($refreshed -gt 0) {
    Write-Host "[wt] refreshed $refreshed stale link(s) after a relink in the primary"
}
if ($refreshFailures -gt 0) {
    Write-Host "[wt] WARNING: $refreshFailures link(s) could not be refreshed - re-run this script while nothing is building in the primary or this worktree."
}

if (-not (Get-ChildItem -LiteralPath $worktreeBin -Filter *.lib -File -ErrorAction SilentlyContinue)) {
    Write-Host "[wt] WARNING: no godot-cpp .lib available to link - the worktree will build godot-cpp itself unless GODOT_CPP_PREBUILT is set."
}
exit 0
