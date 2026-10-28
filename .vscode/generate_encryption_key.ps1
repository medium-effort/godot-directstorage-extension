# Generates a 256-bit cryptographically secure symmetric encryption key for DSPkg archives (Version 4).
# Saves the 64-hex character key to .keys/dspkg_encryption.key (UTF-8 without BOM).
# Displays the key, copies it to the clipboard, and pauses until Enter is pressed.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Position = 0)]
    [string]$KeyFile = ".keys/dspkg_encryption.key",

    [Parameter()]
    [switch]$Force,

    [Parameter()]
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"

# Determine workspace root (script resides in <WorkspaceRoot>/.vscode)
$WorkspaceRoot = Split-Path -Parent $PSScriptRoot
if (-not $WorkspaceRoot) {
    $WorkspaceRoot = (Get-Location).Path
}

# Resolve target key file path relative to workspace root unless rooted
$resolvedKeyFile = if ([System.IO.Path]::IsPathRooted($KeyFile)) {
    $KeyFile
} else {
    [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($WorkspaceRoot, $KeyFile))
}

# Relative display path for logs
$displayRelPath = if ($resolvedKeyFile.StartsWith($WorkspaceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    $resolvedKeyFile.Substring($WorkspaceRoot.Length).TrimStart('\', '/')
} else {
    $resolvedKeyFile
}

# Overwrite guard
if ([System.IO.File]::Exists($resolvedKeyFile) -and -not $Force) {
    Write-Host ""
    Write-Host "[WARNING] Key file already exists at:" -ForegroundColor Yellow
    Write-Host "  $resolvedKeyFile" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Overwriting will make previously built .dspkg archives unreadable with the new key!" -ForegroundColor Yellow
    Write-Host ""
    $response = Read-Host "Do you want to overwrite it? (y/N)"
    if ($response -notmatch '^(y|yes)$') {
        Write-Host "Operation cancelled. Existing key file was not modified." -ForegroundColor DarkGray
        if (-not $NoPause) {
            Write-Host ""
            $null = Read-Host "Press Enter to exit..."
        }
        exit 0
    }
}

# Ensure parent directory exists
$targetDir = [System.IO.Path]::GetDirectoryName($resolvedKeyFile)
if ($targetDir -and -not [System.IO.Directory]::Exists($targetDir)) {
    if ($PSCmdlet.ShouldProcess($targetDir, "Create key directory")) {
        [System.IO.Directory]::CreateDirectory($targetDir) | Out-Null
    }
}

# Generate 32 cryptographically secure random bytes (256 bits)
$bytes = [byte[]]::new(32)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($bytes)
$keyHex = (-join ($bytes | ForEach-Object { $_.ToString("x2") })).ToLowerInvariant()

# Write to file strictly using UTF-8 without BOM
if ($PSCmdlet.ShouldProcess($resolvedKeyFile, "Write 64-hex encryption key")) {
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($resolvedKeyFile, $keyHex, $utf8NoBom)
}

# Attempt to copy to clipboard for convenience
$copiedToClipboard = $false
try {
    Set-Clipboard -Value $keyHex -ErrorAction Stop
    $copiedToClipboard = $true
} catch {
    $copiedToClipboard = $false
}

# Attempt to derive BLAKE2b-64 key ID fingerprint via python if available
$keyId = $null
$pyCmd = Get-Command python -ErrorAction SilentlyContinue
if ($pyCmd) {
    try {
        $pyArgs = @("-c", "import hashlib, sys; print(hashlib.blake2b(bytes.fromhex('$keyHex'), digest_size=8).hexdigest().lower())")
        $idOutput = & $pyCmd.Source $pyArgs 2>$null
        if ($idOutput -and ($idOutput.Trim().Length -eq 16)) {
            $keyId = $idOutput.Trim()
        }
    } catch {
        $keyId = $null
    }
}

# Display summary in terminal
Clear-Host
Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "                DIRECTSTORAGE ENCRYPTION KEY GENERATED                          " -ForegroundColor Green
Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  File saved to:" -ForegroundColor Gray
Write-Host "    $resolvedKeyFile" -ForegroundColor White
Write-Host ""
Write-Host "  Symmetric 256-bit Key (64 hex characters):" -ForegroundColor Gray
Write-Host "  +------------------------------------------------------------------------+" -ForegroundColor Cyan
Write-Host "  | $keyHex |" -ForegroundColor Yellow
Write-Host "  +------------------------------------------------------------------------+" -ForegroundColor Cyan
Write-Host ""
if ($keyId) {
    Write-Host "  Key Fingerprint (16-hex BLAKE2b Key ID):" -ForegroundColor Gray
    Write-Host "    $keyId" -ForegroundColor Cyan
    Write-Host ""
}
if ($copiedToClipboard) {
    Write-Host "  [OK] Key has been automatically copied to your clipboard!" -ForegroundColor Green
} else {
    Write-Host "  [!] Please select and copy the 64-hex key above to your clipboard." -ForegroundColor Yellow
}
Write-Host ""
Write-Host "  Where to use this key (legacy source build and local tooling only):" -ForegroundColor Gray
Write-Host "    * SCons (.env)  : DSPKG_ENCRYPTION_KEY_FILE=$displayRelPath" -ForegroundColor White
Write-Host "    * GDScript      : DSPkg.set_global_encryption_key(`"$keyHex`")" -ForegroundColor White
Write-Host "  Shipped games do not use a fixed key: every export generates a random archive key" -ForegroundColor Gray
Write-Host "  and delivers it inside the encrypted PCK." -ForegroundColor Gray
Write-Host ""
Write-Host "  Security Reminder:" -ForegroundColor Gray
Write-Host "    Never commit this key file to public source control (ignored by .gitignore)." -ForegroundColor DarkGray
Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host ""

if (-not $NoPause) {
    Write-Host "  After copying your key, press Enter to close this terminal..." -ForegroundColor Magenta
    $null = Read-Host
}
