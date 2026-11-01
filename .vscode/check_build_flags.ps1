<#
.SYNOPSIS
    Verifies that every translation unit is compiled with the MSVC hardening flags.

.DESCRIPTION
    This extension compiles attacker-reachable C - libdeflate's GDeflate decoder and
    Monocypher - as well as C++. SCons expands $CFLAGS/$CCFLAGS on the C compile line and
    $CXXFLAGS/$CCFLAGS on the C++ line, so a hardening flag that lives only in CXXFLAGS
    never reaches a .c file. The project therefore carries /guard:cf and /Qspectre in
    CCFLAGS, and this gate proves it by reading the command lines SCons actually emits.

    The check regenerates the compilation database (`scons ... compiledb`) and inspects
    every entry: each C and C++ command line must carry both flags, and neither flag may
    appear twice (a duplicate means the flags were copied into CCFLAGS while being left
    in CXXFLAGS). It reads the emitted command, not SConstruct, so it fails when the
    build system stops emitting the flags for any reason - which a text search cannot do.

    The database is deleted before SCons runs: a stale file would let a failed or
    flag-less build pass.

    Output is plain (no colours; the exit code is the verdict) so .vscode/build_remote.ps1
    can capture and re-emit it without polluting stdout in -Json mode:
        exit 0  every translation unit is hardened
        exit 1  the invariant is violated (unhardened units, or duplicated flags)
        exit 2  the check could not run (no compiler database, or SCons failed)

.PARAMETER Root
    Repository root. Defaults to the parent directory of this script.

.PARAMETER SconsPlatform
    SCons platform argument. Defaults to windows; the flags under test are MSVC-only.

.PARAMETER SconsExe
    SCons executable or command name. Defaults to scons.

.PARAMETER KeepDb
    Leave compile_commands.json in place instead of deleting it afterwards.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .vscode/check_build_flags.ps1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$Root = "",

    [Parameter(Mandatory = $false)]
    [string]$SconsPlatform = "windows",

    [Parameter(Mandatory = $false)]
    [string]$SconsExe = "scons",

    [Parameter(Mandatory = $false)]
    [switch]$KeepDb
)

$ErrorActionPreference = "Stop"

$HardeningFlags = @("/guard:cf", "/Qspectre")
$DbName = "compile_commands.json"

function Write-HardeningError {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Output ("[hardening] ERROR: " + $Message)
}

function Get-FlagCount {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string]$Flag
    )
    # MSVC switches are case-insensitive, and one copy in CXXFLAGS plus one in CCFLAGS is
    # the specific regression this count exists to catch.
    return [System.Text.RegularExpressions.Regex]::Matches(
        $Command,
        [System.Text.RegularExpressions.Regex]::Escape($Flag),
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    ).Count
}

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent $PSScriptRoot
}
$Root = (Resolve-Path -LiteralPath $Root).Path

if (-not (Test-Path -LiteralPath (Join-Path $Root "SConstruct") -PathType Leaf)) {
    Write-HardeningError "no SConstruct under '$Root' - point -Root at a checkout of this extension."
    exit 2
}

if (-not (Get-Command $SconsExe -ErrorAction SilentlyContinue)) {
    Write-HardeningError "'$SconsExe' is not on PATH - the compilation database cannot be generated."
    exit 2
}

$DbPath = Join-Path $Root $DbName

# Vacuous-pass guard: never let a previous run's database answer for this one.
if (Test-Path -LiteralPath $DbPath) {
    Remove-Item -LiteralPath $DbPath -Force
}

$SconsOutput = @()
$SconsExit = 0
Push-Location -LiteralPath $Root
try {
    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $SconsOutput = @(& $SconsExe "platform=$SconsPlatform" "compiledb" "-Q" 2>&1 | ForEach-Object { $_.ToString() })
    $SconsExit = $LASTEXITCODE
    $ErrorActionPreference = $previousEap
}
finally {
    Pop-Location
}

if ($SconsExit -ne 0) {
    Write-HardeningError "scons platform=$SconsPlatform compiledb failed (exit $SconsExit); the flag set could not be read."
    foreach ($line in ($SconsOutput | Select-Object -Last 20)) {
        if ($line.Trim()) { Write-Output ("[hardening]   " + $line.TrimEnd()) }
    }
    # The prebuilt-godot-cpp guard is an environment problem, not a hardening regression.
    if (($SconsOutput -join "`n") -match "shares godot-cpp") {
        Write-HardeningError "this checkout shares godot-cpp with the primary worktree but GODOT_CPP_PREBUILT is not set; build from a bootstrapped worktree, or set GODOT_CPP_PREBUILT=1 in .env."
    }
    exit 2
}

if (-not (Test-Path -LiteralPath $DbPath -PathType Leaf)) {
    Write-HardeningError "scons succeeded but '$DbName' was not written, so nothing was verified."
    exit 2
}

try {
    # NOTE: ConvertFrom-Json emits a JSON array as ONE pipeline object. Wrapping that in
    # @(...) yields a single element that happens to be an array, which silently collapses
    # every count below to 1 - so assign first, then normalise.
    $Parsed = Get-Content -LiteralPath $DbPath -Raw | ConvertFrom-Json
    $Entries = @($Parsed | ForEach-Object { $_ })
}
catch {
    Write-HardeningError ("'" + $DbName + "' is not valid JSON: " + $_.Exception.Message)
    exit 2
}

# Classify by the source file's extension. `output` is an .obj path and must not be used.
$CEntries = @($Entries | Where-Object { ([string]$_.file) -match '\.c$' })
$CppEntries = @($Entries | Where-Object { ([string]$_.file) -match '\.(cpp|cc|cxx)$' })

if ($CEntries.Count -eq 0 -or $CppEntries.Count -eq 0) {
    Write-HardeningError ("the database holds " + $CEntries.Count + " C and " + $CppEntries.Count + " C++ entries; expected both sets. Nothing was verified.")
    exit 2
}

$Offenders = New-Object System.Collections.Generic.List[string]
$MissingUnits = 0
$DuplicateUnits = 0

foreach ($entry in @($CEntries) + @($CppEntries)) {
    $command = [string]$entry.command
    $file = [string]$entry.file
    $missing = @()
    $duplicates = @()
    foreach ($flag in $HardeningFlags) {
        $count = Get-FlagCount -Command $command -Flag $flag
        if ($count -eq 0) {
            $missing += $flag
        }
        elseif ($count -gt 1) {
            $duplicates += ($flag + " x" + $count)
        }
    }
    if ($missing.Count -gt 0) { $MissingUnits++ }
    if ($duplicates.Count -gt 0) { $DuplicateUnits++ }
    if (($missing.Count -gt 0) -or ($duplicates.Count -gt 0)) {
        $detail = @()
        if ($missing.Count -gt 0) { $detail += ("missing " + ($missing -join ", ")) }
        if ($duplicates.Count -gt 0) { $detail += ("duplicate " + ($duplicates -join ", ")) }
        $Offenders.Add($file + " (" + ($detail -join "; ") + ")")
    }
}

if ((-not $KeepDb) -and (Test-Path -LiteralPath $DbPath)) {
    Remove-Item -LiteralPath $DbPath -Force
}

$VerdictFlags = $HardeningFlags -join " and "
$TotalUnits = $CEntries.Count + $CppEntries.Count

if ($Offenders.Count -eq 0) {
    Write-Output ("[hardening] " + $CEntries.Count + " C and " + $CppEntries.Count + " C++ translation unit(s) compile with " + $VerdictFlags + ", 0 duplicate flag(s).")
    exit 0
}

Write-HardeningError ("the hardening invariant is violated in " + $Offenders.Count + " of " + $TotalUnits + " translation unit(s): " + $MissingUnits + " missing-flag unit(s), " + $DuplicateUnits + " duplicate-flag unit(s).")
foreach ($offender in ($Offenders | Select-Object -First 10)) {
    Write-Output ("[hardening]   offender: " + $offender)
}
if ($Offenders.Count -gt 10) {
    Write-Output ("[hardening]   ... and " + ($Offenders.Count - 10) + " more")
}
Write-Output ("[hardening] fix the Windows/MSVC flag block in SConstruct (CCFLAGS carries the code-generation flags for both C and C++); do not weaken this gate.")
exit 1
