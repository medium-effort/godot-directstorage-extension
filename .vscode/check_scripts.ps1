<#
.SYNOPSIS
    Runs headless GDScript syntax validation across addon scripts.

.DESCRIPTION
    Validates all GDScript (*.gd) files in the DirectStorage accelerator addon
    against Godot Engine in headless mode (--check-only --headless) within the
    project context (--path project), ensuring GDExtension symbols and export
    plugins parse without errors.

    Imports the project first when it has never been opened (delegated to
    tools/ensure_project_imported.ps1), because GDExtension symbols only resolve
    after the editor has written .godot/extension_list.cfg. Without that, every
    script touching a class reports a parse error on a fresh checkout.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$GodotPath = "",

    [Parameter(Mandatory = $false)]
    [string]$ProjectDir = "project",

    [Parameter(Mandatory = $false)]
    [string]$ScriptsDir = "project/addons/directstorage_accelerator/scripts",

    [Parameter(Mandatory = $false)]
    [switch]$VerboseOutput
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# 1. Resolve Godot Console Executable
function Resolve-GodotExecutable {
    param([string]$ExplicitPath, [string]$Root)

    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath)) {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    if ($env:GODOT_CONSOLE_PATH -and (Test-Path -LiteralPath $env:GODOT_CONSOLE_PATH)) {
        return (Resolve-Path -LiteralPath $env:GODOT_CONSOLE_PATH).Path
    }

    if ($env:GODOT -and (Test-Path -LiteralPath $env:GODOT)) {
        return (Resolve-Path -LiteralPath $env:GODOT).Path
    }

    # Check .env in repo root
    $envFile = Join-Path $Root ".env"
    if (Test-Path -LiteralPath $envFile) {
        $lines = Get-Content -LiteralPath $envFile -ErrorAction SilentlyContinue
        foreach ($line in $lines) {
            $trimmed = $line.Trim()
            if (-not $trimmed -or $trimmed.StartsWith("#")) { continue }
            if ($trimmed -match '^GODOT_CONSOLE_PATH\s*=\s*(.*)$') {
                $val = $matches[1].Trim().Trim('"').Trim("'")
                if ($val -and (Test-Path -LiteralPath $val)) {
                    return (Resolve-Path -LiteralPath $val).Path
                }
            }
        }
    }

    # Check .agents/PATH_ALLOWLIST.md
    $allowlistFile = Join-Path $Root ".agents\PATH_ALLOWLIST.md"
    if (Test-Path -LiteralPath $allowlistFile) {
        $lines = Get-Content -LiteralPath $allowlistFile -ErrorAction SilentlyContinue
        foreach ($line in $lines) {
            $trimmed = $line.Trim()
            if (-not $trimmed -or $trimmed.StartsWith("#") -or $trimmed.StartsWith("<")) { continue }
            if (Test-Path -LiteralPath $trimmed) {
                if ((Get-Item -LiteralPath $trimmed) -is [System.IO.DirectoryInfo]) {
                    $consoleExe = Get-ChildItem -Path $trimmed -Filter "*console*.exe" -File -ErrorAction SilentlyContinue | Select-Object -First 1
                    if ($consoleExe) {
                        return $consoleExe.FullName
                    }
                } elseif ($trimmed -match 'console' -and $trimmed.EndsWith(".exe", [System.StringComparison]::OrdinalIgnoreCase)) {
                    return (Resolve-Path -LiteralPath $trimmed).Path
                }
            }
        }
    }

    # Fallback to PATH
    foreach ($cmd in @("godot_console", "godot4_console", "godot")) {
        $found = Get-Command $cmd -ErrorAction SilentlyContinue
        if ($found -and $found.Source) {
            return $found.Source
        }
    }

    return $null
}

$resolvedGodot = Resolve-GodotExecutable -ExplicitPath $GodotPath -Root $repoRoot
if (-not $resolvedGodot) {
    Write-Error "[check_scripts] Could not locate Godot console executable. Set GODOT_CONSOLE_PATH or check .env."
    exit 1
}

$absProjectDir = if ([System.IO.Path]::IsPathRooted($ProjectDir)) { $ProjectDir } else { Join-Path $repoRoot $ProjectDir }
$absScriptsDir = if ([System.IO.Path]::IsPathRooted($ScriptsDir)) { $ScriptsDir } else { Join-Path $repoRoot $ScriptsDir }

if (-not (Test-Path -LiteralPath $absProjectDir)) {
    Write-Error "[check_scripts] Project directory not found at '$absProjectDir'."
    exit 1
}

if (-not (Test-Path -LiteralPath $absScriptsDir)) {
    Write-Error "[check_scripts] Scripts directory not found at '$absScriptsDir'."
    exit 1
}

$scriptFiles = @(Get-ChildItem -LiteralPath $absScriptsDir -Filter "*.gd" -File | Sort-Object Name)
if ($scriptFiles.Count -eq 0) {
    Write-Warning "[check_scripts] No GDScript (*.gd) files found in '$absScriptsDir'."
    exit 0
}

# GDExtension classes resolve only when the project has been imported once: without
# .godot/extension_list.cfg, --check-only reports "Identifier ... not declared" for
# every script that touches a class, which looks like a code break but is an
# unimported checkout (a fresh clone or worktree). Import instead of failing.
$importHelper = Join-Path $repoRoot "tools\ensure_project_imported.ps1"
if (Test-Path -LiteralPath $importHelper) {
    $registryPath = Join-Path $absProjectDir ".godot\extension_list.cfg"
    if (-not (Test-Path -LiteralPath $registryPath)) {
        Write-Host "[check_scripts] This checkout has never been imported; importing once so GDExtension classes resolve..." -ForegroundColor Yellow
    }
    & powershell -ExecutionPolicy Bypass -File $importHelper -ProjectDir $absProjectDir -GodotPath $resolvedGodot -Quiet
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[check_scripts] Project import failed, so GDExtension classes stay unresolved; stopping here instead of reporting misleading parse errors." -ForegroundColor Red
        exit 1
    }
}

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host " [GDScript Parse Check] Validating $($scriptFiles.Count) addon script(s)..." -ForegroundColor White
Write-Host " Godot Binary : $resolvedGodot" -ForegroundColor DarkGray
Write-Host " Project Root : $absProjectDir" -ForegroundColor DarkGray
Write-Host "================================================================================" -ForegroundColor Cyan

$failedScripts = @()
$totalCount = $scriptFiles.Count
$passedCount = 0

foreach ($file in $scriptFiles) {
    # Compute relative path from project root using forward slashes
    $fullPath = $file.FullName
    $relFromProj = $fullPath
    if ($fullPath.StartsWith($absProjectDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        $relFromProj = $fullPath.Substring($absProjectDir.Length).TrimStart("\", "/")
    }
    $relFromProj = $relFromProj -replace '\\', '/'

    if ($VerboseOutput) {
        Write-Host "Checking: $relFromProj" -ForegroundColor DarkGray
    }

    # Native stderr (plugin warnings, driver notices) must not abort the check.
    # Under $ErrorActionPreference = "Stop", a redirected native stderr write
    # becomes a terminating NativeCommandError before $LASTEXITCODE is read, so
    # the whole script exits 1 without ever reporting a parse verdict. The verdict
    # is the exit code plus the SCRIPT ERROR / Parse Error text below; warnings are
    # not parse failures.
    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $rawOutput = & $resolvedGodot --path $absProjectDir --headless --check-only --script $relFromProj 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousEap
    }
    $outString = ($rawOutput | Out-String).Trim()

    $hasParseError = ($exitCode -ne 0) -or ($outString -match 'SCRIPT ERROR' -or $outString -match 'Parse Error')

    if ($hasParseError) {
        Write-Host "  [FAIL] $relFromProj" -ForegroundColor Red
        if ($outString) {
            foreach ($line in ($outString -split "`r?`n")) {
                if ($line.Trim()) {
                    Write-Host "         $line" -ForegroundColor Yellow
                }
            }
        }
        $failedScripts += [PSCustomObject]@{
            File   = $file.Name
            Path   = $relFromProj
            Output = $outString
        }
    } else {
        $passedCount++
        Write-Host "  [PASS] $relFromProj" -ForegroundColor Green
    }
}

Write-Host "--------------------------------------------------------------------------------" -ForegroundColor Cyan
if ($failedScripts.Count -gt 0) {
    Write-Host " [PARSE CHECK FAILED] $($failedScripts.Count) of $totalCount script(s) failed syntax validation." -ForegroundColor White -BackgroundColor DarkRed
    Write-Host "================================================================================" -ForegroundColor Red
    exit 1
} else {
    Write-Host " [PARSE CHECK PASSED] All $passedCount script(s) passed GDScript validation." -ForegroundColor White -BackgroundColor DarkGreen
    Write-Host "================================================================================" -ForegroundColor Green
    exit 0
}
