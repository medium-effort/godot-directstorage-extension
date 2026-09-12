param(
    [string]$Target = "editor",
    [switch]$IncludePdb
)

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
    Write-Host "[ERROR] Configuration file '.env' not found!" -ForegroundColor Red
    Write-Host "Please copy '.env.example' to '.env' and configure REMOTE_HOST and REMOTE_DIR." -ForegroundColor Yellow
    exit 1
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

$RemoteHost = $env:REMOTE_HOST
$RemoteDir  = $env:REMOTE_DIR

if (-not $RemoteHost -or -not $RemoteDir) {
    Write-Host "[ERROR] REMOTE_HOST or REMOTE_DIR is missing in $EnvFilePath" -ForegroundColor Red
    exit 1
}

$ArtifactRemoteDir = "$RemoteDir/project/addons/directstorage_accelerator/bin"
$ArtifactLocalDir  = "./project/addons/directstorage_accelerator/bin"

# 1. Ensure target directory exists on Device B
Write-Host "[1/4] Ensuring build directory exists on Device B..." -ForegroundColor Cyan
ssh -4 $RemoteHost "if not exist `"$RemoteDir`" mkdir `"$RemoteDir`""

# 2. Stream source files via native tar over SSH (using cmd.exe for raw binary piping)
Write-Host "[2/4] Syncing source files to Device B..." -ForegroundColor Cyan
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

cmd.exe /c "tar -czf - $tarExcludes . | ssh -4 $RemoteHost ""tar -xzf - -C $RemoteDir"""

if ($LASTEXITCODE -ne 0) {
    Write-Host "Sync to Device B failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

# 3. Trigger SCons build on Device B
Write-Host "[3/4] Compiling target=$Target on Device B..." -ForegroundColor Cyan
ssh -4 $RemoteHost "cd /d $RemoteDir && call scons platform=windows target=$Target dev_build=yes & exit /b %ERRORLEVEL%"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed on Device B!" -ForegroundColor Red
    exit $LASTEXITCODE
}

# 4. Pull back compiled binary and debug symbols to Device A
Write-Host "[4/4] Pulling artifacts back to Device A (compressed)..." -ForegroundColor Cyan
if (!(Test-Path $ArtifactLocalDir)) {
    New-Item -ItemType Directory -Force -Path $ArtifactLocalDir | Out-Null
}

# Pull runtime DLLs and manifest with on-the-fly compression (-C)
scp -4 -C "$RemoteHost`:$ArtifactRemoteDir/*.dll" "$ArtifactLocalDir/"
scp -4 -C "$RemoteHost`:$ArtifactRemoteDir/build_manifest.json" "$ArtifactLocalDir/" 2>$null

# Pull large PDB debug symbols only when explicitly requested
if ($IncludePdb) {
    Write-Host "Pulling debug symbols (.pdb) with compression..." -ForegroundColor Cyan
    scp -4 -C "$RemoteHost`:$ArtifactRemoteDir/*.pdb" "$ArtifactLocalDir/" 2>$null
} else {
    Write-Host "Skipping .pdb download (use -IncludePdb if attaching a C++ debugger)." -ForegroundColor DarkGray
}

Write-Host "Done! Artifacts updated on Device A." -ForegroundColor Green

