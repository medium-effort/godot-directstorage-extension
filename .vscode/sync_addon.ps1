# Syncs build output into the external project(s) without building.
#
# The implementation lives in tools/sync_artifacts.py (shared with
# build_remote.ps1 step [6/6]); this wrapper only resolves the configuration and
# forwards arguments. Use it when only GDScript or non-C++ addon files changed
# and the C++ DLLs are already up to date.
#
#   .\sync_addon.ps1                     -> SYNC_ADDON_DIR (default)
#   .\sync_addon.ps1 -Targets all        -> test, copyable and addon targets
#   .\sync_addon.ps1 -Targets copyable   -> SYNC_ADDON_COPYABLE_DIR only
#   .\sync_addon.ps1 -DryRun             -> report what would change
param(
    [switch]$Verbose,
    [string]$Targets = "addon",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$EnvFilePath = Join-Path $RepoRoot ".env"
$SyncScript = Join-Path $RepoRoot "tools\sync_artifacts.py"

if (-not (Test-Path -LiteralPath $EnvFilePath)) {
    Write-Host "Configuration file '.env' not found! Please copy '.env.example' to '.env' and configure SYNC_ADDON_DIR." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $SyncScript)) {
    Write-Host "Sync implementation not found: $SyncScript" -ForegroundColor Red
    exit 1
}

$SyncArgs = @($SyncScript, "--targets", $Targets)
if ($Verbose) { $SyncArgs += "--verbose" }
if ($DryRun) { $SyncArgs += "--dry-run" }

& python @SyncArgs
exit $LASTEXITCODE
