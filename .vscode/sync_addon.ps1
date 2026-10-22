# Syncs project/addons/directstorage_accelerator -> $env:SYNC_ADDON_DIR without building.
# Use when only GDScript/non-C++ addon files changed (C++ DLLs already up to date).
# Copies only files that are new or newer (size differs OR source LastWriteTime > dest + 1s).
param(
    [switch]$Verbose
)

$ScriptStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Write-VerboseHost {
    param(
        [string]$Message,
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray
    )
    if ($Verbose) {
        Write-Host $Message -ForegroundColor $ForegroundColor
    }
}

# Load configuration from .env file (same candidates as build_remote.ps1)
$EnvFileCandidates = @(
    (Join-Path $PSScriptRoot "..\.env"),
    (Join-Path $PWD ".env"),
    (Join-Path $PSScriptRoot "sync_addon.env")
)

$EnvFilePath = $null
foreach ($path in $EnvFileCandidates) {
    if (Test-Path $path) {
        $EnvFilePath = (Resolve-Path $path).Path
        break
    }
}

if (-not $EnvFilePath) {
    Write-Host "Configuration file '.env' not found! Please copy '.env.example' to '.env' and configure SYNC_ADDON_DIR." -ForegroundColor Red
    exit 1
}

Get-Content $EnvFilePath | ForEach-Object {
    $line = $_.Trim()
    if ($line -and -not $line.StartsWith("#")) {
        $key, $value = $line -split '=', 2
        if ($key -and $value) {
            [Environment]::SetEnvironmentVariable($key.Trim(), $value.Trim().Trim('"').Trim("'"), "Process")
        }
    }
}

$SyncTargetDir = $env:SYNC_ADDON_DIR
if (-not $SyncTargetDir) {
    Write-Host "SYNC_ADDON_DIR is missing in $EnvFilePath" -ForegroundColor Red
    exit 1
}

if ($Verbose) {
    Write-Host "Loaded config from $EnvFilePath" -ForegroundColor DarkGray
}

$SyncParent = Split-Path -Parent $SyncTargetDir
if ((-not (Test-Path $SyncTargetDir)) -and $SyncParent -and (-not (Test-Path $SyncParent))) {
    try {
        New-Item -ItemType Directory -Force -Path $SyncParent | Out-Null
    } catch {
        Write-Host "Sync target parent is not accessible: $SyncParent" -ForegroundColor Yellow
        exit 0
    }
}

$LocalAddonDir = (Resolve-Path (Join-Path $PSScriptRoot "..\project\addons\directstorage_accelerator")).Path
if (-not (Test-Path $LocalAddonDir)) {
    Write-Host "Local addon dir not found: $LocalAddonDir" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $SyncTargetDir)) {
    New-Item -ItemType Directory -Force -Path $SyncTargetDir | Out-Null
}

function Copy-SafeFile {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )
    if ($DestinationPath.ToLower().EndsWith(".dll")) {
        $oldPath = "$DestinationPath.old"
        if (Test-Path $oldPath) {
            try { Remove-Item -Force -Path $oldPath -ErrorAction SilentlyContinue } catch { $null = $_ }
        }
    }
    try {
        Copy-Item -Path $SourcePath -Destination $DestinationPath -Force -ErrorAction Stop
    }
    catch {
        if ((Test-Path $DestinationPath) -and $DestinationPath.ToLower().EndsWith(".dll")) {
            $oldPath = "$DestinationPath.old"
            if (Test-Path $oldPath) {
                try { Remove-Item -Force -Path $oldPath -ErrorAction SilentlyContinue } catch { $null = $_ }
            }
            if (Test-Path $oldPath) {
                $timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
                $oldPath = "$DestinationPath.$timestamp.old"
            }
            try {
                Move-Item -Path $DestinationPath -Destination $oldPath -Force -ErrorAction SilentlyContinue
            }
            catch { $null = $_ }
            Copy-Item -Path $SourcePath -Destination $DestinationPath -Force -ErrorAction SilentlyContinue
        }
        else {
            throw $_
        }
    }
}

$copiedCount = 0
$skippedCount = 0
Get-ChildItem -Path $LocalAddonDir -Recurse | ForEach-Object {
    $item = $_
    $relPath = $item.FullName.Substring($LocalAddonDir.Length).TrimStart("\", "/")

    # Exclude temporary, old, and dev build files (same as post-build sync)
    if ($item.Name.StartsWith("~") -or $item.Name.EndsWith(".tmp") -or $item.Name.EndsWith(".old")) {
        return
    }
    if ($relPath.StartsWith("bin") -and $item.Name.Contains(".dev.")) {
        return
    }

    $destPath = Join-Path $SyncTargetDir $relPath
    if ($item.PSIsContainer) {
        if (-not (Test-Path $destPath)) {
            New-Item -ItemType Directory -Force -Path $destPath | Out-Null
        }
    }
    else {
        $destParent = Split-Path $destPath -Parent
        if (-not (Test-Path $destParent)) {
            New-Item -ItemType Directory -Force -Path $destParent | Out-Null
        }

        $needCopy = $false
        if (-not (Test-Path $destPath)) {
            $needCopy = $true
        }
        else {
            $destItem = Get-Item $destPath
            if ($item.Length -ne $destItem.Length -or $item.LastWriteTime -gt $destItem.LastWriteTime.AddSeconds(1)) {
                $needCopy = $true
            }
        }

        if ($needCopy) {
            try {
                Copy-SafeFile -SourcePath $item.FullName -DestinationPath $destPath
                $copiedCount++
                Write-VerboseHost "  synced: $relPath" -ForegroundColor Green
            }
            catch {
                Write-Host "Warning: Failed to copy $($item.Name): $_" -ForegroundColor Yellow
            }
        }
        else {
            $skippedCount++
            Write-VerboseHost "  up-to-date: $relPath" -ForegroundColor DarkGray
        }
    }
}

# Track orphaned files in destination that no longer exist in source addon
$orphanedFiles = [System.Collections.Generic.List[string]]::new()
Get-ChildItem -Path $SyncTargetDir -Recurse -File | ForEach-Object {
    $destFile = $_
    $relPath = $destFile.FullName.Substring($SyncTargetDir.Length).TrimStart("\", "/")

    # Exclude binaries, temporary, and old files
    if ($relPath.StartsWith("bin") -or $destFile.Name.EndsWith(".old") -or $destFile.Name.EndsWith(".tmp")) {
        return
    }

    $sourcePath = Join-Path $LocalAddonDir $relPath
    if (-not (Test-Path $sourcePath)) {
        $orphanedFiles.Add($destFile.FullName)
    }
}

# Attempt to clean up unlocked .old files in destination bin
$destBin = Join-Path $SyncTargetDir "bin"
if (Test-Path $destBin) {
    Get-ChildItem -Path $destBin -Filter "*.old" -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item -Force -Path $_.FullName -ErrorAction SilentlyContinue } catch { $null = $_ }
    }
}

$elapsedSecs = $ScriptStopwatch.Elapsed.TotalSeconds.ToString("F1")
if ($copiedCount -gt 0) {
    Write-Host "Sync complete: Updated $copiedCount file(s), $skippedCount already up to date (${elapsedSecs}s) -> $SyncTargetDir" -ForegroundColor Green
}
else {
    Write-Host "Sync: Target $SyncTargetDir is already up to date ($skippedCount file(s) checked, ${elapsedSecs}s)." -ForegroundColor DarkGray
}

if ($orphanedFiles.Count -gt 0) {
    Write-Host ""
    Write-Host "WARNING: Found $($orphanedFiles.Count) orphaned file(s) in sync target ($SyncTargetDir) that no longer exist in source addon:" -ForegroundColor Yellow
    foreach ($orphan in $orphanedFiles) {
        Write-Host "  - $orphan" -ForegroundColor Yellow
    }
    Write-Host "Please remove them manually if they are no longer needed." -ForegroundColor Yellow
}
