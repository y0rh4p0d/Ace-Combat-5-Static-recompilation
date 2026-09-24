# Standalone launcher for a recompiled Ace Combat 5 folder.
#
# This file is copied into the packaged folder by tools/export_release.ps1 and is
# meant to be run from inside it.  It looks for a disc image next to itself, then
# in the usual places, and starts ac5.exe with the right arguments.
#
#   .\launch.ps1                          # find a disc image automatically
#   .\launch.ps1 -Iso D:\Games\ac5.iso    # or point at one
#   .\launch.ps1 -List                    # just show what it found, do not run
#
# Everything uses paths relative to this script, so the folder can be moved or
# copied anywhere.

[CmdletBinding()]
param(
    [string]$Iso,
    [string[]]$ExtraArgs,
    [switch]$Trace,
    [switch]$List,
    [string]$Log
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $here) { $here = (Get-Location).Path }

$exe = Join-Path $here 'ac5.exe'
if (-not (Test-Path $exe)) {
    Write-Host "ac5.exe is not next to this script ($here)." -ForegroundColor Red
    Write-Host "Run launch.ps1 from inside the folder it was packaged with."
    exit 1
}

# --- which region is this package? ---------------------------------------
# Decided by what is in the folder, so the launcher does not have to be told.
$hasImage = Test-Path (Join-Path $here 'ps2_image.bin')

# --- find a disc image ----------------------------------------------------
function Find-Disc {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        Write-Host "No such file: $Explicit" -ForegroundColor Red
        return $null
    }
    # Next to the launcher first: that is where a user is most likely to drop it.
    $hit = Get-ChildItem $here -Filter '*.iso' -ErrorAction SilentlyContinue |
           Sort-Object Length -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }

    $dirs = @(
        (Join-Path $here 'iso'),
        (Join-Path $here 'disc'),
        (Split-Path $here -Parent),
        (Join-Path $env:USERPROFILE 'Documents'),
        (Join-Path $env:USERPROFILE 'Downloads'),
        (Join-Path $env:USERPROFILE 'Desktop')
    )
    foreach ($d in $dirs) {
        if (-not (Test-Path $d)) { continue }
        $hit = Get-ChildItem $d -Filter '*.iso' -ErrorAction SilentlyContinue |
               Sort-Object Length -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$disc = Find-Disc -Explicit $Iso

# --- report ---------------------------------------------------------------
$arch = 'unknown'
try {
    $b = [System.IO.File]::ReadAllBytes($exe)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    if ($machine -eq 0x8664) { $arch = 'x64' }
    elseif ($machine -eq 0xAA64) { $arch = 'arm64' }
} catch { }

Write-Host ""
Write-Host "  Ace Combat 5 (recompiled)   $arch" -ForegroundColor Cyan
Write-Host "  folder   $here"
Write-Host "  renderer $(if ($hasImage) { 'native (ps2_image.bin present)' } else { 'emulated GS (no ps2_image.bin)' })"
if ($disc) {
    $mb = [math]::Round((Get-Item $disc).Length / 1MB)
    Write-Host "  disc     $(Split-Path $disc -Leaf)  ($mb MB)"
    if ($mb -lt 1000) {
        Write-Host ""
        Write-Host "  This disc image is only $mb MB; a PS2 DVD is over 4 GB, so this is" -ForegroundColor Yellow
        Write-Host "  probably a stub and the game will not get far." -ForegroundColor Yellow
    }
} else {
    Write-Host "  disc     (none found)" -ForegroundColor Yellow
}

if ($arch -eq 'arm64' -and $env:PROCESSOR_ARCHITECTURE -eq 'AMD64') {
    Write-Host ""
    Write-Host "  This is the arm64 build and this PC is x64.  Copy the folder to a" -ForegroundColor Red
    Write-Host "  Windows-on-ARM device instead." -ForegroundColor Red
    exit 1
}

if ($List) { exit 0 }

if (-not $disc) {
    Write-Host ""
    Write-Host "  No disc image found.  Put your ISO in this folder, or pass it:" -ForegroundColor Yellow
    Write-Host "      .\launch.ps1 -Iso ""D:\Games\your-disc.iso""" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  You need your own copy of the game; none is included." -ForegroundColor Yellow
    exit 1
}

# --- run ------------------------------------------------------------------
# --data . because the recompiler output lives next to the exe in a package that
# was exported with -WithImage; without it, --data is only a place to look and
# the runtime says so and carries on.
$a = @('--data', $here, '--disc', $disc, '--watchdog', '0')
if ($Trace) { $a += '--verbose' }
if ($ExtraArgs) { foreach ($x in $ExtraArgs) { $a += $x } }

Write-Host ""
Write-Host "  starting..." -ForegroundColor Green
Write-Host "  the window stays black for about 20 seconds before the first picture" -ForegroundColor DarkGray
Write-Host "  F11 fullscreen   F4 settings   Esc quit" -ForegroundColor DarkGray
Write-Host ""

Push-Location $here
try {
    if ($Log) { & $exe @a 2>&1 | Tee-Object -FilePath $Log }
    else      { & $exe @a }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $code
