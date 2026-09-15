param(
    [string]$Target = "editor",
    [switch]$IncludePdb
)

function Show-BuildFailure {
    param(
        [string]$Step,
        [string]$Message,
        [int]$Code = 1
    )
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host " [BUILD FAILED] $Step " -ForegroundColor White -BackgroundColor DarkRed
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host " Reason: $Message" -ForegroundColor Red
    if ($Code -ne 0) {
        Write-Host " Exit Code: $Code" -ForegroundColor Yellow
    }
    Write-Host " Check the compiler/terminal output above for details." -ForegroundColor DarkGray
    Write-Host "================================================================================" -ForegroundColor Red
    Write-Host ""
    exit $Code
}

function Show-BuildSuccess {
    param(
        [string]$Message = "All steps completed successfully!"
    )
    Write-Host ""
    Write-Host "================================================================================" -ForegroundColor Green
    Write-Host " [BUILD SUCCESS] $Message " -ForegroundColor White -BackgroundColor DarkGreen
    Write-Host "================================================================================" -ForegroundColor Green
    Write-Host ""
}

# Load configuration from .env file
$EnvFileCandidates = @(
    (Join-Path $PSScriptRoot "..\.env"),
    (Join-Path $PWD ".env"),
    (Join-Path $PSScriptRoot "build_remote.env")
)

$EnvFilePath = $null
foreach ($path in $EnvFileCandidates) {
    if (Test-Path $path) {
        $EnvFilePath = (Resolve-Path $path).Path
        break
    }
}

if (-not $EnvFilePath) {
    Show-BuildFailure -Step "Configuration Error" -Message "Configuration file '.env' not found! Please copy '.env.example' to '.env' and configure REMOTE_HOST and REMOTE_DIR." -Code 1
}

# Parse .env file
Get-Content $EnvFilePath | ForEach-Object {
    $line = $_.Trim()
    if ($line -and -not $line.StartsWith("#")) {
        $key, $value = $line -split '=', 2
        if ($key -and $value) {
            [Environment]::SetEnvironmentVariable($key.Trim(), $value.Trim(), "Process")
        }
    }
}

$RemoteHost         = $env:REMOTE_HOST
$RemoteHostFallback = $env:REMOTE_HOST_FALLBACK
$RemoteDir          = $env:REMOTE_DIR

if (-not $RemoteHost -or -not $RemoteDir) {
    Show-BuildFailure -Step "Configuration Error" -Message "REMOTE_HOST or REMOTE_DIR is missing in $EnvFilePath" -Code 1
}

$ArtifactRemoteDir = "$RemoteDir/project/addons/directstorage_accelerator/bin"
$ArtifactLocalDir  = "./project/addons/directstorage_accelerator/bin"

# 1. Probe connectivity (Local Wi-Fi first, fallback to Tailscale)
Write-Host "[1/6] Checking connectivity to primary host ($RemoteHost)..." -ForegroundColor Cyan
$ActiveHost = $null
& ssh -4 -o BatchMode=yes -o ConnectTimeout=2 $RemoteHost "exit 0" 2>$null

if ($LASTEXITCODE -eq 0) {
    $ActiveHost = $RemoteHost
    Write-Host "Connected via local network ($ActiveHost)." -ForegroundColor Green
} elseif ($RemoteHostFallback) {
    Write-Host "Local host unreachable. Attempting Tailscale fallback ($RemoteHostFallback)..." -ForegroundColor Yellow
    & ssh -o BatchMode=yes -o ConnectTimeout=4 $RemoteHostFallback "exit 0" 2>$null
    if ($LASTEXITCODE -eq 0) {
        $ActiveHost = $RemoteHostFallback
        Write-Host "Connected via Tailscale ($ActiveHost)." -ForegroundColor Green
    } else {
        Show-BuildFailure -Step "Step [1/6] Connectivity" -Message "Neither primary host ($RemoteHost) nor fallback ($RemoteHostFallback) could be reached!" -Code 1
    }
} else {
    Show-BuildFailure -Step "Step [1/6] Connectivity" -Message "Cannot reach primary host ($RemoteHost) and no fallback is configured!" -Code 1
}

# 2. Ensure target directory exists on Device B
Write-Host "[2/6] Ensuring build directory exists on Device B..." -ForegroundColor Cyan
ssh $ActiveHost "if not exist `"$RemoteDir`" mkdir `"$RemoteDir`""
if ($LASTEXITCODE -ne 0) {
    Show-BuildFailure -Step "Step [2/6] Directory Setup" -Message "Failed to ensure remote build directory exists on $ActiveHost." -Code $LASTEXITCODE
}

# 3. Stream source files via native tar over SSH (using cmd.exe for raw binary piping)
Write-Host "[3/6] Syncing source files to Device B..." -ForegroundColor Cyan
$tarExcludes = @(
    "--exclude=.git",
    "--exclude=.vscode",
    "--exclude=.codegraph",
    "--exclude=.env*",
    "--exclude=.sconsign*",
    "--exclude=*.obj",
    "--exclude=*.pdb",
    "--exclude=*.exp",
    "--exclude=*.lib",
    "--exclude=*.ilk",
    "--exclude=*.idb",
    "--exclude=project/addons/directstorage_accelerator/bin/directstorage_accelerator*"
) -join " "

cmd.exe /c "tar -czf - $tarExcludes . | ssh $ActiveHost ""tar -xzf - -C $RemoteDir"""

if ($LASTEXITCODE -ne 0) {
    Show-BuildFailure -Step "Step [3/6] Source Sync" -Message "Failed to sync source files to Device B ($ActiveHost)." -Code $LASTEXITCODE
}

# 4. Trigger SCons build on Device B
Write-Host "[4/6] Compiling target=$Target on Device B..." -ForegroundColor Cyan
ssh $ActiveHost "cd /d $RemoteDir && call scons platform=windows target=$Target dev_build=yes & exit /b %ERRORLEVEL%"

if ($LASTEXITCODE -ne 0) {
    Show-BuildFailure -Step "Step [4/6] SCons Compilation" -Message "Compilation failed on Device B ($ActiveHost)." -Code $LASTEXITCODE
}

# 5. Pull back compiled binary and debug symbols to Device A
Write-Host "[5/6] Pulling artifacts back to Device A (compressed)..." -ForegroundColor Cyan
if (!(Test-Path $ArtifactLocalDir)) {
    New-Item -ItemType Directory -Force -Path $ArtifactLocalDir | Out-Null
}

# Pull runtime DLLs and manifest with on-the-fly compression (-C)
scp -C "$ActiveHost`:$ArtifactRemoteDir/*.dll" "$ArtifactLocalDir/"
if ($LASTEXITCODE -ne 0) {
    Show-BuildFailure -Step "Step [5/6] Pull Artifacts" -Message "Failed to copy compiled .dll files from Device B ($ActiveHost)." -Code $LASTEXITCODE
}
scp -C "$ActiveHost`:$ArtifactRemoteDir/build_manifest.json" "$ArtifactLocalDir/" 2>$null

# Pull large PDB debug symbols only when explicitly requested
if ($IncludePdb) {
    Write-Host "Pulling debug symbols (.pdb) with compression..." -ForegroundColor Cyan
    scp -C "$ActiveHost`:$ArtifactRemoteDir/*.pdb" "$ArtifactLocalDir/" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Warning: Failed to download .pdb debug symbols." -ForegroundColor Yellow
    }
} else {
    Write-Host "Skipping .pdb download (use -IncludePdb if attaching a C++ debugger)." -ForegroundColor DarkGray
}

Write-Host "Artifacts updated on Device A." -ForegroundColor Green

# 6. Post-build synchronization to external test project (e.g. TPS demo)
$EnableSyncVal = if ($env:ENABLE_POST_BUILD_ADDON_SYNC) { $env:ENABLE_POST_BUILD_ADDON_SYNC } `
                 elseif ($env:ENABLE_ADDON_SYNC) { $env:ENABLE_ADDON_SYNC } `
                 elseif ($env:DIRECTSTORAGE_ENABLE_EXTERNAL_SYNC) { $env:DIRECTSTORAGE_ENABLE_EXTERNAL_SYNC } `
                 else { "1" }

$ShouldSync = ($EnableSyncVal.Trim().ToLower() -notin @("0", "false", "no", "off"))
$SyncTargetDir = if ($env:SYNC_ADDON_DIR) { $env:SYNC_ADDON_DIR } `
                 else { "<WORKSPACE_ROOT>\godot-tps-directstorage-integration-test\tps-demo\addons\directstorage_accelerator" }

$TargetGuid = if ($env:DEVICE_GUID) { $env:DEVICE_GUID } `
              elseif ($env:TARGET_MACHINE_GUID) { $env:TARGET_MACHINE_GUID } `
              else { "300c220b-66de-4a10-b184-86a6211dfeb2" }

if ($ShouldSync) {
    # Verify machine GUID on Windows
    $CurrentGuid = $null
    try {
        $CurrentGuid = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Cryptography" -ErrorAction SilentlyContinue).MachineGuid
    } catch {
        $CurrentGuid = $null
    }

    if ($TargetGuid -and $CurrentGuid -and ($TargetGuid.Trim().ToLower() -ne $CurrentGuid.Trim().ToLower())) {
        Write-Host "Skipping post-build sync: Machine GUID ($CurrentGuid) does not match target ($TargetGuid)." -ForegroundColor DarkGray
    } elseif (-not (Test-Path "D:\")) {
        Write-Host "Skipping post-build sync: Drive D: not found." -ForegroundColor DarkGray
    } else {
        Write-Host "[6/6] Syncing addon to external project ($SyncTargetDir)..." -ForegroundColor Cyan
        $LocalAddonDir = (Resolve-Path (Join-Path $PSScriptRoot "..\project\addons\directstorage_accelerator")).Path

        if (Test-Path $LocalAddonDir) {
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
                    Copy-Item -Path $SourcePath -Destination $DestinationPath -Force
                } catch {
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
                        } catch { $null = $_ }
                        Copy-Item -Path $SourcePath -Destination $DestinationPath -Force
                    } else {
                        throw $_
                    }
                }
            }

            $copiedCount = 0
            Get-ChildItem -Path $LocalAddonDir -Recurse | ForEach-Object {
                $item = $_
                $relPath = $item.FullName.Substring($LocalAddonDir.Length).TrimStart("\", "/")

                # Exclude temporary, old, and dev build files
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
                } else {
                    $destParent = Split-Path $destPath -Parent
                    if (-not (Test-Path $destParent)) {
                        New-Item -ItemType Directory -Force -Path $destParent | Out-Null
                    }

                    $needCopy = $false
                    if (-not (Test-Path $destPath)) {
                        $needCopy = $true
                    } else {
                        $destItem = Get-Item $destPath
                        if ($item.Length -ne $destItem.Length -or $item.LastWriteTime -gt $destItem.LastWriteTime.AddSeconds(1)) {
                            $needCopy = $true
                        }
                    }

                    if ($needCopy) {
                        try {
                            Copy-SafeFile -SourcePath $item.FullName -DestinationPath $destPath
                            $copiedCount++
                        } catch {
                            Write-Host "Warning: Failed to copy $($item.Name): $_" -ForegroundColor Yellow
                        }
                    }
                }
            }

            # Attempt to clean up unlocked .old files in destination bin
            $destBin = Join-Path $SyncTargetDir "bin"
            if (Test-Path $destBin) {
                Get-ChildItem -Path $destBin -Filter "*.old" -ErrorAction SilentlyContinue | ForEach-Object {
                    try { Remove-Item -Force -Path $_.FullName -ErrorAction SilentlyContinue } catch { $null = $_ }
                }
            }

            # Update build_manifest.json on Device A using Python for clean 2-space indent (avoiding PS 5.1 column spacing and BOM)
            $manifestLocalPath = Join-Path $LocalAddonDir "bin\build_manifest.json"
            if (Test-Path $manifestLocalPath) {
                try {
                    $pyCmd = "import json, sys; p = sys.argv[1]; d = json.load(open(p, encoding='utf-8-sig')); d.setdefault('post_build_sync', {}).setdefault('tps_demo_addon', {})['synced'] = True; d['post_build_sync']['tps_demo_addon']['copied_files_count'] = int(sys.argv[2]); json.dump(d, open(p, 'w', encoding='utf-8'), indent=2)"
                    & python -c $pyCmd $manifestLocalPath $copiedCount

                    $destManifest = Join-Path $destBin "build_manifest.json"
                    if (Test-Path $destBin) {
                        Copy-Item -Path $manifestLocalPath -Destination $destManifest -Force
                    }
                } catch {
                    $null = $_
                }
            }

            if ($copiedCount -gt 0) {
                Write-Host "Post-build sync complete: Updated $copiedCount file(s) -> $SyncTargetDir" -ForegroundColor Green
            } else {
                Write-Host "Post-build sync: Target $SyncTargetDir is already up to date." -ForegroundColor DarkGray
            }
        }
    }
} else {
    Write-Host "Post-build addon sync is disabled (ENABLE_POST_BUILD_ADDON_SYNC=0)." -ForegroundColor DarkGray
}

Show-BuildSuccess -Message "All build and sync steps completed successfully!"


