<#
.SYNOPSIS
    Materialize a worktree-local .env for a parallel-worktree build.

.DESCRIPTION
    Invoked by the pre-start hook in .config/wt.toml. Every worktree needs its own
    .env (gitignored, so a fresh worktree has none), but three values must differ
    from the primary copy:

      ENABLE_POST_BUILD_ADDON_SYNC=0
          SYNC_ADDON_DIR points at one shared addon folder outside every
          worktree. Leaving it enabled makes N parallel builds overwrite each
          other's DLLs there, and the last writer silently wins.

      SCONS_CACHE=<primary>\.cache\scons
          The shared object cache, and the reason a new worktree's first build is
          cheap. SCons signatures are content/command based and path independent,
          so an object compiled in one worktree is retrieved by another - verified
          across two worktrees. Objects are written next to their sources with no
          shared variant dir, so without this every worktree recompiles all of
          godot-cpp from scratch (~3,100 translation units).

    Everything else is copied verbatim from the primary worktree's .env, which
    stays the single source of truth (REMOTE_BUILD, PREVENT_BUILD_ON_THIS_DEVICE,
    GODOT_CONSOLE_PATH, DEVICE_GUID, ...).

.NOTE
    sccache (USE_SCCACHE=1) is NOT set here. Measured: it cannot share hits
    across worktrees, because it hashes MSVC's preprocessed output and that
    carries `#line "D:\\...\\<worktree>\\src\\..."` entries which
    SCCACHE_BASEDIRS cannot strip and /pathmap does not rewrite. It remains
    useful inside a single worktree; enable it manually if you want that.

.PARAMETER PrimaryWorktreePath
    Path of the primary worktree. The hook passes {{ primary_worktree_path }},
    which is absolute. A relative path is accepted and canonicalized, because
    every value derived from it (SCONS_CACHE, SCCACHE_DIR) is written to the
    generated .env as an absolute path.

.PARAMETER DestinationPath
    .env to write. Defaults to the current directory (the hook runs at the new
    worktree's root). A relative path is canonicalized against the current
    directory.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PrimaryWorktreePath,
    [string]$DestinationPath = (Join-Path (Get-Location).Path ".env"),
    [switch]$GodotCppPrebuilt
)

$ErrorActionPreference = "Stop"

# Canonicalize both paths before deriving anything from them. SCONS_CACHE and
# SCCACHE_DIR are absolute-intent values: godot-cpp hands SCONS_CACHE to
# CacheDir(), and the root SConstruct joins "build-signature.json" onto it. A
# relative value therefore means a different directory depending on who resolves
# it, which either fails the build in CacheDir's mkdir or silently fragments the
# shared cache (#0220). Resolve-Path also normalizes forward slashes and drops a
# trailing separator, so the TrimEnd below only has to cover what it preserves.
if (-not (Test-Path -LiteralPath $PrimaryWorktreePath -PathType Container)) {
    Write-Error "Primary worktree not found at '$PrimaryWorktreePath'. Pass the path of the primary checkout (absolute, or relative to the current directory)."
    exit 1
}
$primaryRoot = (Resolve-Path -LiteralPath $PrimaryWorktreePath).Path.TrimEnd('\', '/')
$sourcePath = Join-Path $primaryRoot ".env"

# The destination .env does not exist yet in a fresh worktree, so it cannot be
# resolved by existence; this yields an absolute path without touching disk.
$DestinationPath = [System.IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DestinationPath))

if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    Write-Error "Primary .env not found at '$sourcePath'. Copy .env.example to .env in the primary worktree before creating worktrees."
    exit 1
}

$sconsCacheDir = Join-Path $primaryRoot ".cache\scons"
$sccacheDir = Join-Path $primaryRoot ".cache\sccache"

# Only claim prebuilt mode when a library actually exists in this worktree: the
# SConstruct hard-fails on GODOT_CPP_PREBUILT=1 without it, and a worktree whose
# sharing step was skipped must fall back to building godot-cpp itself.
$prebuiltAvailable = $false
if ($GodotCppPrebuilt) {
    $libs = @(Get-ChildItem -LiteralPath (Join-Path (Split-Path -Path $DestinationPath -Parent) "godot-cpp\bin") -Filter *.lib -File -ErrorAction SilentlyContinue)
    $prebuiltAvailable = $libs.Count -gt 0
    if (-not $prebuiltAvailable) {
        Write-Host "[wt] -GodotCppPrebuilt requested but no godot-cpp .lib is linked here; this worktree will build godot-cpp itself."
    }
}

# Order matters only for readability of the generated file.
$overrides = [ordered]@{
    "ENABLE_POST_BUILD_ADDON_SYNC" = "0"
    "SCONS_CACHE"                  = $sconsCacheDir
}
if ($prebuiltAvailable) { $overrides["GODOT_CPP_PREBUILT"] = "1" }
else { $overrides["GODOT_CPP_PREBUILT"] = "0" }

$primaryLines = @(Get-Content -LiteralPath $sourcePath)

# The opt-in sccache knobs stay omitted: they are not the shared cache (see the
# .NOTE in the header), and writing them here would silently change debug info
# from /Zi to /Z7 in every worktree.
if (-not ($primaryLines | Where-Object { $_ -match '^\s*SCCACHE_DIR\s*=' })) {
    $overrides["SCCACHE_DIR"] = $sccacheDir
}

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("# Generated for worktree '$(Split-Path -Leaf $DestinationPath)' by tools/wt_bootstrap_env.ps1.")
$lines.Add("# Source of truth: '$sourcePath'. Edit that file, not this one.")
$lines.Add("")
foreach ($line in $primaryLines) { $lines.Add($line) }

foreach ($key in @($overrides.Keys)) {
    $pattern = '^\s*' + [regex]::Escape($key) + '\s*='
    $replaced = $false
    for ($i = 3; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $pattern) {
            $lines[$i] = "$key=$($overrides[$key])"
            $replaced = $true
            break
        }
    }
    if (-not $replaced) { $lines.Add("$key=$($overrides[$key])") }
}

$parentDir = Split-Path -Path $DestinationPath -Parent
if ($parentDir -and -not (Test-Path -LiteralPath $parentDir -PathType Container)) {
    [System.IO.Directory]::CreateDirectory($parentDir) | Out-Null
}
[System.IO.File]::WriteAllLines($DestinationPath, $lines, [System.Text.UTF8Encoding]::new($false))

Write-Host "[wt] wrote $DestinationPath (addon sync off, shared SCons cache -> $sconsCacheDir)"
exit 0
