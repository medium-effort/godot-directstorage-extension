<#
.SYNOPSIS
    Runs a headless startup smoke test against the template_release GDExtension binary.

.DESCRIPTION
    Packs a minimal Godot project (project.godot + directstorage_accelerator.gdextension)
    into build/release_smoke/smoke.pck using PCKPacker, copies the resolved
    template_release Godot executable alongside the release addon binaries, and runs
    it with --headless --quit to verify MODULE_INITIALIZATION_LEVEL_SCENE succeeds
    without crashes (e.g. 0xC0000005) or missing-singleton errors.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$GodotConsolePath = "",

    [Parameter(Mandatory = $false)]
    [string]$TemplateReleasePath = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Get-EnvOrDotEnvValue {
    param([string]$Name, [string]$Root)
    $ambient = [System.Environment]::GetEnvironmentVariable($Name)
    if ($ambient) {
        return $ambient.Trim()
    }
    $envFile = Join-Path $Root ".env"
    if (Test-Path -LiteralPath $envFile) {
        foreach ($line in (Get-Content -LiteralPath $envFile -ErrorAction SilentlyContinue)) {
            $trimmed = $line.Trim()
            if (-not $trimmed -or $trimmed.StartsWith("#")) { continue }
            if ($trimmed -match ("^" + [regex]::Escape($Name) + "\s*=\s*(.*)$")) {
                return $matches[1].Trim().Trim('"').Trim("'")
            }
        }
    }
    return ""
}

function Resolve-GodotConsole {
    param([string]$ExplicitPath, [string]$Root)
    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath)) {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    foreach ($candidate in @(
        (Get-EnvOrDotEnvValue -Name "GODOT_CONSOLE_PATH" -Root $Root),
        (Get-EnvOrDotEnvValue -Name "GODOT" -Root $Root)
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $cmd = Get-Command "godot" -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    throw "[release-smoke] Could not locate Godot console executable. Set GODOT_CONSOLE_PATH in .env or environment."
}

function Get-GodotExportTemplateVersion {
    param([string]$GodotExe)
    $rawVersion = (& $GodotExe --version 2>&1 | Select-Object -First 1)
    if (-not $rawVersion) {
        throw "[release-smoke] '$GodotExe --version' returned empty output."
    }
    $ver = $rawVersion.ToString().Trim()
    if ($ver -match '^(\d+\.\d+(?:\.\d+)?\.[A-Za-z0-9_-]+?)(?:\.(?:official|custom_build)\..+)?$') {
        return $matches[1]
    }
    return $ver
}

function Resolve-TemplateReleaseExecutable {
    param([string]$ExplicitPath, [string]$GodotExe, [string]$Root)

    $custom = if ($ExplicitPath) { $ExplicitPath } else { Get-EnvOrDotEnvValue -Name "GODOT_TEMPLATE_RELEASE_PATH" -Root $Root }
    if ($custom) {
        if (-not (Test-Path -LiteralPath $custom)) {
            throw "[release-smoke] GODOT_TEMPLATE_RELEASE_PATH does not exist: '$custom'"
        }
        return (Resolve-Path -LiteralPath $custom).Path
    }

    $versionDir = Get-GodotExportTemplateVersion -GodotExe $GodotExe
    $isWin = ($env:OS -eq "Windows_NT")
    $isMac = $false
    if (-not $isWin) {
        $uname = (& uname -s 2>$null)
        if ($uname -and $uname.ToString().Trim() -eq "Darwin") {
            $isMac = $true
        }
    }

    if ($isWin) {
        $baseDir = Join-Path $env:APPDATA "Godot\export_templates\$versionDir"
        $candidates = @("windows_release_x86_64.exe", "windows_release_x86_64_console.exe")
    } elseif ($isMac) {
        $baseDir = Join-Path $HOME "Library/Application Support/Godot/export_templates/$versionDir"
        $candidates = @("macos_template.app/Contents/MacOS/Godot", "macos.app/Contents/MacOS/Godot")
    } else {
        $xdgData = if ($env:XDG_DATA_HOME) { $env:XDG_DATA_HOME } else { Join-Path $HOME ".local/share" }
        $baseDir = Join-Path $xdgData "godot/export_templates/$versionDir"
        $candidates = @("linux_release.x86_64", "linux_release.arm64")
    }

    foreach ($rel in $candidates) {
        $full = Join-Path $baseDir $rel
        if (Test-Path -LiteralPath $full) {
            return (Resolve-Path -LiteralPath $full).Path
        }
    }

    throw "[release-smoke] Could not find template_release binary for Godot '$versionDir' under '$baseDir'. Install the export template or set GODOT_TEMPLATE_RELEASE_PATH."
}

$godotConsole = Resolve-GodotConsole -ExplicitPath $GodotConsolePath -Root $repoRoot
$templateExe = Resolve-TemplateReleaseExecutable -ExplicitPath $TemplateReleasePath -GodotExe $godotConsole -Root $repoRoot

$smokeDir = Join-Path $repoRoot "build\release_smoke"
$smokeBinDir = Join-Path $smokeDir "addons\directstorage_accelerator\bin"

try {
New-Item -ItemType Directory -Force -Path $smokeBinDir | Out-Null

$addonDir = Join-Path $repoRoot "project\addons\directstorage_accelerator"
$gdextSrc = Join-Path $addonDir "directstorage_accelerator.gdextension"
$binSrcDir = Join-Path $addonDir "bin"

if (-not (Test-Path -LiteralPath $gdextSrc)) {
    throw "[release-smoke] Missing gdextension manifest: '$gdextSrc'"
}

# Copy release runtime binaries (exclude debug/symbol/import artifacts).
Get-ChildItem -LiteralPath $binSrcDir -File | Where-Object {
    $_.Name -notmatch '\.debug\.' -and $_.Extension -notin @(".pdb", ".lib", ".exp", ".ilk", ".tmp")
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $smokeBinDir $_.Name) -Force
}

$smokeExeName = if ($env:OS -eq "Windows_NT") { "smoke.exe" } else { "smoke" }
$smokeExe = Join-Path $smokeDir $smokeExeName
Copy-Item -LiteralPath $templateExe -Destination $smokeExe -Force

$smokeProj = Join-Path $smokeDir "project.godot"
$smokeProjText = @"
config_version=5

[application]
config/name="dsgd_release_smoke"
run/main_scene="res://smoke.tscn"
"@
[System.IO.File]::WriteAllText($smokeProj, $smokeProjText, [System.Text.UTF8Encoding]::new($false))

$extList = Join-Path $smokeDir "extension_list.cfg"
[System.IO.File]::WriteAllText($extList, "res://addons/directstorage_accelerator/directstorage_accelerator.gdextension`n", [System.Text.UTF8Encoding]::new($false))

$scriptCache = Join-Path $smokeDir "global_script_class_cache.cfg"
[System.IO.File]::WriteAllText($scriptCache, "list=[]`n", [System.Text.UTF8Encoding]::new($false))

$smokeMain = Join-Path $smokeDir "smoke_main.gd"
$smokeMainCode = @"
extends Node
func _ready() -> void:
	if not Engine.has_singleton("DSContext") or not Engine.has_singleton("DSTextureCache"):
		push_error("DirectStorage singletons not registered in template_release.")
		get_tree().quit(2)
		return
	print("RELEASE_SMOKE_OK")
	get_tree().quit(0)
"@
[System.IO.File]::WriteAllText($smokeMain, $smokeMainCode, [System.Text.UTF8Encoding]::new($false))

$smokeTscn = Join-Path $smokeDir "smoke.tscn"
$smokeTscnText = @"
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://smoke_main.gd" id="1"]

[node name="Smoke" type="Node"]
script = ExtResource("1")
"@
[System.IO.File]::WriteAllText($smokeTscn, $smokeTscnText, [System.Text.UTF8Encoding]::new($false))

$smokePck = Join-Path $smokeDir "smoke.pck"
$packerScript = Join-Path $smokeDir "pack_smoke.gd"
$packerCode = @"
extends SceneTree
func _init() -> void:
	var packer := PCKPacker.new()
	var err := packer.pck_start("$($smokePck.Replace('\', '/'))")
	if err != OK:
		push_error("pck_start failed: %d" % err)
		quit(1)
		return
	packer.add_file("res://project.godot", "$($smokeProj.Replace('\', '/'))")
	packer.add_file("res://.godot/extension_list.cfg", "$($extList.Replace('\', '/'))")
	packer.add_file("res://.godot/global_script_class_cache.cfg", "$($scriptCache.Replace('\', '/'))")
	packer.add_file("res://smoke.tscn", "$($smokeTscn.Replace('\', '/'))")
	packer.add_file("res://smoke_main.gd", "$($smokeMain.Replace('\', '/'))")
	packer.add_file("res://addons/directstorage_accelerator/directstorage_accelerator.gdextension", "$($gdextSrc.Replace('\', '/'))")
	err = packer.flush()
	if err != OK:
		push_error("pck flush failed: %d" % err)
		quit(1)
		return
	quit(0)
"@
[System.IO.File]::WriteAllText($packerScript, $packerCode, [System.Text.UTF8Encoding]::new($false))

$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$packOut = & $godotConsole --headless --path $smokeDir --script $packerScript 2>&1
$packExit = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($packExit -ne 0) {
    $packOut | ForEach-Object { Write-Host $_ }
    throw "[release-smoke] Failed to build minimal smoke.pck (exit $packExit)."
}

$stdoutLog = Join-Path $smokeDir "smoke.stdout.log"
$stderrLog = Join-Path $smokeDir "smoke.stderr.log"
if (Test-Path -LiteralPath $stdoutLog) { Remove-Item -LiteralPath $stdoutLog -Force }
if (Test-Path -LiteralPath $stderrLog) { Remove-Item -LiteralPath $stderrLog -Force }

Write-Host "[release-smoke] Running template_release smoke check ($templateExe)..." -ForegroundColor Cyan
$proc = Start-Process -FilePath $smokeExe `
    -ArgumentList @("--headless") `
    -WorkingDirectory $smokeDir `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -NoNewWindow `
    -PassThru
$null = $proc.Handle

if (-not $proc.WaitForExit(30000)) {
    try { $proc.Kill() } catch { Write-Verbose "Smoke process kill ignored: $_" }
    throw "[release-smoke] template_release smoke process timed out after 30s."
}
$proc.WaitForExit()

$exitCode = $proc.ExitCode
$stdoutText = if (Test-Path -LiteralPath $stdoutLog) { [System.IO.File]::ReadAllText($stdoutLog) } else { "" }
$stderrText = if (Test-Path -LiteralPath $stderrLog) { [System.IO.File]::ReadAllText($stderrLog) } else { "" }

if ($exitCode -ne 0) {
    Write-Host "--- template_release stderr ---" -ForegroundColor Red
    if ($stderrText) { Write-Host $stderrText }
    throw "[release-smoke] template_release smoke process failed with exit code $exitCode."
}

if ($stdoutText -notmatch "RELEASE_SMOKE_OK") {
    Write-Host "--- template_release stdout ---" -ForegroundColor Red
    if ($stdoutText) { Write-Host $stdoutText }
    throw "[release-smoke] template_release smoke script did not emit RELEASE_SMOKE_OK."
}

if ($stderrText -match "Failed to retrieve non-existent singleton|Can't open dynamic library") {
    Write-Host "--- template_release stderr ---" -ForegroundColor Red
    Write-Host $stderrText
    throw "[release-smoke] template_release smoke check detected startup error in stderr."
}

Write-Host "[release-smoke] PASS: template_release booted and exited cleanly (code 0)." -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $smokeDir) {
        Remove-Item -LiteralPath $smokeDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
