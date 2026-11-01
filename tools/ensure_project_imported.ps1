<#
.SYNOPSIS
    Make sure a Godot project checkout has been imported, so GDExtension classes resolve.

.DESCRIPTION
    A checkout that has never been opened by the editor has no
    `.godot/extension_list.cfg`. Without it, `godot --check-only --script <file>`
    resolves no GDExtension classes, so every script that touches one reports a
    parse error that has nothing to do with the code - for example
    `Parse Error: Identifier "DSPkg" not declared in ds_dspkg_static.gd` in a
    fresh worktree. Running the editor import once writes that registry.

    Idempotent: a no-op when the registry already exists. Judge success by the
    registry file rather than the exit code, because a cold first import can end
    in the known teardown abort from engine issue #0169 *after* the import has
    completed.

    Lives in tools/ rather than .vscode/ because it is a build utility, not a
    task: build verification calls it, and a person or agent can run it directly
    before a build that skips verification.

.PARAMETER ProjectDir
    Project directory, absolute or relative to the repository root. Default: project.

.PARAMETER GodotPath
    Godot console executable. Defaults to GODOT_CONSOLE_PATH, then GODOT, then the
    GODOT_CONSOLE_PATH entry in the repository's .env.

.PARAMETER TimeoutSec
    Upper bound on the import. Default: 180.

.PARAMETER Quiet
    Print only on failure.

.OUTPUTS
    Exit code 0 when the extension registry exists afterwards, 1 otherwise.

.NOTES
    Reports through exit codes, so invoke it as a child process
    (`powershell -File tools/ensure_project_imported.ps1 ...`). Running it
    in-process (`& tools/ensure_project_imported.ps1`) would let its `exit`
    terminate the caller instead of just this helper.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$ProjectDir = "project",

    [Parameter(Mandatory = $false)]
    [string]$GodotPath = "",

    [Parameter(Mandatory = $false)]
    [int]$TimeoutSec = 180,

    [Parameter(Mandatory = $false)]
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projectAbs = if ([System.IO.Path]::IsPathRooted($ProjectDir)) { $ProjectDir } else { Join-Path $repoRoot $ProjectDir }
$registry = Join-Path $projectAbs ".godot\extension_list.cfg"

if (-not (Test-Path -LiteralPath $projectAbs)) {
    Write-Error "[ensure-project-imported] Project directory not found at '$projectAbs'."
    exit 1
}

if (Test-Path -LiteralPath $registry) {
    if (-not $Quiet) { Write-Host "[ensure-project-imported] already imported: $registry" -ForegroundColor DarkGray }
    exit 0
}

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

    $envFile = Join-Path $Root ".env"
    if (Test-Path -LiteralPath $envFile) {
        foreach ($line in (Get-Content -LiteralPath $envFile -ErrorAction SilentlyContinue)) {
            $trimmed = $line.Trim()
            if (-not $trimmed -or $trimmed.StartsWith("#")) { continue }
            if ($trimmed -match '^GODOT_CONSOLE_PATH\s*=\s*(.*)$') {
                $val = $matches[1].Trim().Trim('"').Trim("'")
                if ($val -and (Test-Path -LiteralPath $val)) { return (Resolve-Path -LiteralPath $val).Path }
            }
        }
    }
    return $null
}

$godot = Resolve-GodotExecutable -ExplicitPath $GodotPath -Root $repoRoot
if (-not $godot) {
    Write-Error "[ensure-project-imported] Could not locate a Godot console executable. Set GODOT_CONSOLE_PATH or check .env."
    exit 1
}

if (-not $Quiet) {
    Write-Host "[ensure-project-imported] No extension registry in '$projectAbs'; importing once..." -ForegroundColor Yellow
}

$stdout = Join-Path $env:TEMP ("ensure_import_out_" + [System.Guid]::NewGuid().ToString() + ".log")
$stderr = Join-Path $env:TEMP ("ensure_import_err_" + [System.Guid]::NewGuid().ToString() + ".log")
try {
    # --editor --quit-after: the editor's first scan is what writes the registry.
    # --quit alone does not import, and --import exited immediately without
    # producing the file on the device this was measured on.
    $proc = Start-Process -FilePath $godot -ArgumentList @("--headless", "--editor", "--quit-after", "4", "--path", $projectAbs) -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Write-Warning "[ensure-project-imported] Import did not finish within ${TimeoutSec}s; stopped it."
    }
}
finally {
    $importLog = @()
    if (Test-Path -LiteralPath $stdout) { $importLog += @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue); Remove-Item -LiteralPath $stdout -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $stderr) { $importLog += @(Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue); Remove-Item -LiteralPath $stderr -Force -ErrorAction SilentlyContinue }
}

if (Test-Path -LiteralPath $registry) {
    if (-not $Quiet) { Write-Host "[ensure-project-imported] Imported; registry written: $registry" -ForegroundColor Green }
    exit 0
}

Write-Error "[ensure-project-imported] The import did not produce '$registry'. GDExtension classes will not resolve in this checkout."
if ($importLog) {
    Write-Host "--- import output (tail) ---" -ForegroundColor Yellow
    $importLog | Select-Object -Last 15 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkYellow }
}
exit 1
