# Generates compile_commands.json for clangd language server.
# By default, generates locally with SCons on this machine.
# Use -Remote (or REMOTE_BUILD=1 in .env) to offload generation to a remote
# host where MSVC and SCons are configured, then download and normalize paths.
# Use -Local to explicitly force local generation.

param(
    [switch]$Local,
    [switch]$Remote,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"

function Write-VerboseHost {
    param(
        [string]$Message,
        [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray
    )
    if ($Verbose) {
        Write-Host $Message -ForegroundColor $ForegroundColor
    }
}

$WorkspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$CompileCommandsLocal = Join-Path $WorkspaceRoot "compile_commands.json"

# Load configuration from .env file (for REMOTE_BUILD / REMOTE_HOST / REMOTE_DIR)
$EnvFileCandidates = @(
    (Join-Path $WorkspaceRoot ".env"),
    (Join-Path $PSScriptRoot "..\.env"),
    (Join-Path $PSScriptRoot "build_remote.env")
)

$EnvFilePath = $null
foreach ($cand in $EnvFileCandidates) {
    if (Test-Path -LiteralPath $cand) {
        $EnvFilePath = (Resolve-Path -LiteralPath $cand).Path
        break
    }
}

if ($EnvFilePath) {
    Get-Content -LiteralPath $EnvFilePath | ForEach-Object {
        $line = $_.Trim()
        if ($line -and -not $line.StartsWith("#")) {
            $key, $value = $line -split '=', 2
            if ($key -and $value) {
                $k = $key.Trim()
                if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($k, "Process"))) {
                    [Environment]::SetEnvironmentVariable($k, $value.Trim().Trim('"').Trim("'"), "Process")
                }
            }
        }
    }
}

$IsRemote = ($Remote) -or (-not $Local -and ($env:REMOTE_BUILD -in @("1", "true", "yes")))

if (-not $IsRemote) {
    Write-Host "[1/2] Generating compile_commands.json locally..." -ForegroundColor Cyan
    Push-Location $WorkspaceRoot
    try {
        & scons platform=windows compiledb
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Error: Local SCons invocation failed with exit code $LASTEXITCODE." -ForegroundColor Red
            exit $LASTEXITCODE
        }
    }
    catch {
        Write-Host "Error: Failed to invoke scons locally: $_" -ForegroundColor Red
        exit 1
    }
    finally {
        Pop-Location
    }

    if (Test-Path -LiteralPath $CompileCommandsLocal) {
        Write-Host "[2/2] compile_commands.json generated locally successfully!" -ForegroundColor Green
        exit 0
    }
    else {
        Write-Host "Error: compile_commands.json not found at $CompileCommandsLocal." -ForegroundColor Red
        exit 1
    }
}

# -----------------------------------------------------------------------------
# Remote Generation (REMOTE_BUILD=1 or -Remote)
# -----------------------------------------------------------------------------

if (-not $EnvFilePath) {
    Write-Host "Configuration file '.env' not found! Please ensure REMOTE_HOST and REMOTE_DIR are configured." -ForegroundColor Red
    exit 1
}

$RemoteHost = $env:REMOTE_HOST
$RemoteHostFallback = $env:REMOTE_HOST_FALLBACK
$RemoteDir = $env:REMOTE_DIR

if (-not $RemoteHost -or -not $RemoteDir) {
    Write-Host "Error: REMOTE_HOST or REMOTE_DIR missing from $EnvFilePath." -ForegroundColor Red
    exit 1
}

$RemoteDirWin = $RemoteDir.Replace('/', '\')
$RemoteDirTar = $RemoteDir.Replace('\', '/')

# Determine active host
Write-Host "[1/5] Checking connectivity to remote host..." -ForegroundColor Cyan
$ActiveHost = $null
$pingPrimary = Test-Connection -ComputerName ($RemoteHost -split '@')[-1] -Count 1 -Quiet
if ($pingPrimary) {
    $ActiveHost = $RemoteHost
    Write-VerboseHost "Connected to primary host ($ActiveHost)." -ForegroundColor Green
}
elseif ($RemoteHostFallback) {
    $pingFallback = Test-Connection -ComputerName ($RemoteHostFallback -split '@')[-1] -Count 1 -Quiet
    if ($pingFallback) {
        $ActiveHost = $RemoteHostFallback
        Write-VerboseHost "Primary unreachable, connected to fallback ($ActiveHost)." -ForegroundColor Yellow
    }
}

if (-not $ActiveHost) {
    Write-Host "Error: Neither primary ($RemoteHost) nor fallback ($RemoteHostFallback) could be reached!" -ForegroundColor Red
    exit 1
}

# Sync recent sources so SCons detects all files
Write-Host "[2/5] Syncing source files to remote host..." -ForegroundColor Cyan
# Product stem for the extension binaries (see ADDON_BINARY_STEM in SConstruct).
$AddonBinaryStem = "dsgd"
$tarExcludesList = @(
    "--exclude=.git",
    "--exclude=.vscode",
    "--exclude=.codegraph",
    "--exclude=.opencode",
    "--exclude=.agents",
    "--exclude=.crewmate",
    "--exclude=.godot",
    "--exclude=godot-cpp",
    "--exclude=.env*",
    "--exclude=.sconsign*",
    "--exclude=*.obj",
    "--exclude=*.pdb",
    "--exclude=*.exp",
    "--exclude=*.lib",
    "--exclude=*.ilk",
    "--exclude=*.idb",
    "--exclude=src/gen/dspkg_encryption_key.h*",
    "--exclude=.keys",
    "--exclude=*.key",
    "--exclude=*.key.txt",
    "--exclude=project/addons/directstorage_accelerator/bin/$AddonBinaryStem*",
    "--exclude=project/addons/directstorage_accelerator/bin/~*",
    "--exclude=sync_manifest.tmp",
    "--exclude=./wavexecute_tasks"
)

$KeyFileRelWin = ""
if ($env:DSPKG_ENCRYPTION_KEY_FILE) {
    $candKeyFile = if ([System.IO.Path]::IsPathRooted($env:DSPKG_ENCRYPTION_KEY_FILE)) {
        $env:DSPKG_ENCRYPTION_KEY_FILE
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $WorkspaceRoot $env:DSPKG_ENCRYPTION_KEY_FILE))
    }
    $rootWithSep = $WorkspaceRoot.TrimEnd('\', '/') + '\'
    if ($candKeyFile.StartsWith($rootWithSep, [System.StringComparison]::OrdinalIgnoreCase)) {
        $rel = $candKeyFile.Substring($rootWithSep.Length)
        $KeyFileRelWin = $rel.Replace('/', '\')
        $tarExcludesList += ("--exclude=" + $rel.Replace('\', '/'))
    }
}

$tarExcludes = $tarExcludesList -join " "

try {
    $tarCmd = "tar -czf - $tarExcludes . | ssh -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost ""tar -xzf - -C $RemoteDirTar"""
    cmd.exe /c $tarCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Warning: Fast source sync returned code $LASTEXITCODE; continuing with remote database generation." -ForegroundColor Yellow
    }

    # Execute SCons compiledb on remote host
    Write-Host "[3/5] Generating compilation database on remote host..." -ForegroundColor Cyan
    $RemoteMarker = "$RemoteDirWin\compiledb.ok"
    & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "del /f /q `"$RemoteMarker`" 2>nul"

    $remoteCmd = "cd /d `"$RemoteDirWin`" && call scons platform=windows compiledb && echo ok> `"$RemoteMarker`""
    & ssh -o BatchMode=yes -o ConnectTimeout=10 $ActiveHost $remoteCmd

    & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "type `"$RemoteMarker`" >nul 2>nul"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Remote SCons compiledb failed on remote host." -ForegroundColor Red
        exit 1
    }

    # Download compile_commands.json
    Write-Host "[4/5] Downloading compile_commands.json from remote host..." -ForegroundColor Cyan
    $RemoteCompiledbTar = "$RemoteDirTar/compile_commands.json"
    & scp -4 -o BatchMode=yes -o ConnectTimeout=5 "$ActiveHost`:$RemoteCompiledbTar" "$CompileCommandsLocal"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $CompileCommandsLocal)) {
        Write-Host "Error: Failed to transfer compile_commands.json via SCP." -ForegroundColor Red
        exit 1
    }

    # Normalize paths for local clangd
    Write-Host "[5/5] Normalizing paths in compile_commands.json for local workspace..." -ForegroundColor Cyan

    $RemoteWinUpper = $RemoteDirWin
    $RemoteTarUpper = $RemoteDirTar
    $LocalWin = $WorkspaceRoot
    $LocalTar = $WorkspaceRoot.Replace('\', '/')

    $content = [System.IO.File]::ReadAllText($CompileCommandsLocal, [System.Text.Encoding]::UTF8)

    # Replace remote base directory variations with local base directory using literal string replaces
    # 1. Escaped JSON backslashes: D:\\ssh-tunnel\\builds\\godot-directstorage -> D:\\Users\\...
    $remoteJsonEscaped = $RemoteWinUpper.Replace('\', '\\')
    $localJsonEscaped = $LocalWin.Replace('\', '\\')
    $content = $content.Replace($remoteJsonEscaped, $localJsonEscaped)

    # 2. Forward slashes: D:/ssh-tunnel/builds/godot-directstorage -> D:/Users/...
    $content = $content.Replace($RemoteTarUpper, $LocalTar)

    # 3. Single backslashes (if unescaped): D:\ssh-tunnel\builds\godot-directstorage -> D:\Users\...
    $content = $content.Replace($RemoteWinUpper, $LocalWin)

    # 4. Clean up any quadruple backslashes if present
    $quadBackslashes = $LocalWin.Replace('\', '\\\\')
    if ($content.Contains($quadBackslashes)) {
        $content = $content.Replace($quadBackslashes, $localJsonEscaped)
    }

    [System.IO.File]::WriteAllText($CompileCommandsLocal, $content, [System.Text.Encoding]::UTF8)

    Write-Host "compile_commands.json successfully created and configured at $CompileCommandsLocal!" -ForegroundColor Green
    exit 0
}
finally {
    if ($ActiveHost) {
        $headerWin = "$RemoteDirWin\src\gen\dspkg_encryption_key.h"
        $remotePurgeTargets = "`"$headerWin*`" `"$headerWin.tmp*`""
        if ($KeyFileRelWin) { $remotePurgeTargets += " `"$RemoteDirWin\$KeyFileRelWin`"" }
        & ssh -n -o BatchMode=yes -o ConnectTimeout=5 $ActiveHost "del /f /q $remotePurgeTargets 2>nul" 2>&1 | Out-Null
    }
}
