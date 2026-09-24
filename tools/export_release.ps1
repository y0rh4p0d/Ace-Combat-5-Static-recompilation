# Package a built game into a folder that runs on its own.
#
#   pwsh -File tools/export_release.ps1 -Region cnjp
#   pwsh -File tools/export_release.ps1 -Region us -Arch arm64
#   pwsh -File tools/export_release.ps1 -Region cnjp -Out D:\AC5
#
# The result is one folder holding ac5.exe, the DLLs and shaders it needs, the
# recompiler's memory image, and a launcher, so it can be copied to another
# machine and started by double-clicking.  The person running it supplies their
# own disc image, which is why the launcher asks for one and why `--disc` is not
# baked in.
#
# ABOUT ps2_image.bin.  It is included, because the program will not start
# without it -- a build launched without it stalls during boot and shows nothing
# but a black window.  It is also a copy of the game's own executable, taken from
# whatever executable was recompiled.  That makes this package fine for your own
# use and for testing, but it is not something to hand out publicly: it carries
# game code, and this project does not redistribute any.  If you want to publish
# a build, publish the source and have people recompile, which produces the image
# on their own machine.

[CmdletBinding()]
param(
    [ValidateSet('cnjp', 'us', 'both')]
    [string]$Region = 'cnjp',

    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',

    # Where to put the folder.  Default: dist/ac5-<region>-<arch>
    [string]$Out,

    # Skip building; use whatever is already in build/.
    [switch]$NoBuild,

    [switch]$Force,   # overwrite an existing output folder

    # Acknowledge that this package contains game code and is for personal use.
    [switch]$PersonalUseOnly
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path

if (-not $PersonalUseOnly) {
    Write-Host ""
    Write-Host "This packages ps2_image.bin, which is a copy of the game's executable." -ForegroundColor Yellow
    Write-Host "The folder will run on its own, but it carries game code, so keep it for" -ForegroundColor Yellow
    Write-Host "your own use and do not distribute it.  Re-run with -PersonalUseOnly to" -ForegroundColor Yellow
    Write-Host "confirm you understand that." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

function Export-One([string]$region, [string]$arch) {
    $buildDir = "build/$arch-$region"
    $genDir   = if ($region -eq 'us') { 'generated' } else { 'generated-cnjp' }
    $src      = Join-Path $root $buildDir
    $dest     = if ($Out) { $Out } else { Join-Path $root "dist/ac5-$region-$arch" }

    Write-Host ""
    Write-Host "== packaging $region ($arch)" -ForegroundColor Cyan

    if (-not (Test-Path (Join-Path $src 'ac5.exe'))) {
        if ($NoBuild) { throw "no build in $buildDir; drop -NoBuild to build it first" }
        Write-Host "-- not built yet, building it"
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build_all.ps1') `
               -What $(if ($region -eq 'us') { 'us' } elseif ($arch -eq 'arm64') { 'arm64' } else { 'cnjp' })
        if ($LASTEXITCODE -ne 0) { throw "build failed for $region/$arch" }
    }

    if (Test-Path $dest) {
        if (-not $Force) {
            throw "$dest already exists.  Pass -Force to replace it, or -Out to pick another folder."
        }
        Remove-Item -Recurse -Force $dest
    }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    # --- what the program cannot start without -----------------------------
    # ps2_image.bin is in this list because it really is required: without it the
    # game stalls in its first second and the window stays black.
    $imgSrc = Join-Path $root "$genDir/ps2_image.bin"
    $need = @(
        @{ from = (Join-Path $src 'ac5.exe');              what = 'ac5.exe' },
        @{ from = (Join-Path $src 'SDL3.dll');             what = 'SDL3.dll' },
        @{ from = (Join-Path $src 'libwinpthread-1.dll');  what = 'libwinpthread-1.dll' },
        @{ from = $imgSrc;                                 what = "$genDir/ps2_image.bin" }
    )
    foreach ($n in $need) {
        if (-not (Test-Path $n.from)) {
            if ($n.what -like '*ps2_image.bin') {
                throw "missing $($n.what).  It is written by the recompile step; run tools/build_all.ps1 first."
            }
            throw "missing $($n.what) in $buildDir; run tools/build_all.ps1 first"
        }
        Copy-Item $n.from $dest -Force
        $mb = [math]::Round((Get-Item $n.from).Length / 1MB, 1)
        Write-Host "   + $($n.what)  ($mb MB)"
    }

    # shaders: the game looks for these next to the exe
    $shaders = Join-Path $src 'shaders'
    if (-not (Test-Path $shaders)) { throw "missing shaders/ in $buildDir" }
    Copy-Item $shaders $dest -Recurse -Force
    $nspv = (Get-ChildItem (Join-Path $dest 'shaders') -Filter *.spv | Measure-Object).Count
    Write-Host "   + shaders/  ($nspv .spv)"

    # --- the launcher ------------------------------------------------------
    Copy-Item (Join-Path $PSScriptRoot 'launch.ps1') $dest -Force
    Copy-Item (Join-Path $root 'README.zh-CN.md') (Join-Path $dest 'README.zh-CN.md') -Force
    Copy-Item (Join-Path $root 'LICENSE') $dest -Force
    Write-Host "   + launch.ps1, README.zh-CN.md, LICENSE"

    # A note next to the exe, since that is where someone will look.
    $note = @"
Ace Combat 5 -- recompiled build ($region, $arch)

Double-click launch.cmd (or run launch.ps1) to start.
You need your own disc image (ISO); the launcher will ask for it.

Do not delete ps2_image.bin.  The program will not start without it: it would
stall during boot and show nothing but a black window.

This folder contains no game assets -- models, textures, sound and the movies all
come off your own disc image at run time.  ps2_image.bin is a copy of the game's
executable code taken from the disc you recompiled, so keep this folder to
yourself; it is not something to hand out or upload.

请双击 launch.cmd（或运行 launch.ps1）启动，需要自备光盘镜像 ISO。

请勿删除 ps2_image.bin：没有它程序无法启动，会在启动阶段卡住并一直黑屏。

本文件夹不含任何游戏素材——模型、贴图、音效和影像都在运行时从你自己的光盘镜像读取。
ps2_image.bin 是从你所重编译的光盘中取出的游戏可执行代码，因此请勿分发或上传本文件夹。
"@
    Set-Content (Join-Path $dest 'README.txt') $note -Encoding UTF8
    Write-Host "   + README.txt"

    # A .cmd so it can be started by double-clicking, without a PowerShell window
    # closing on an error before it can be read.
    $cmd = @"
@echo off
setlocal
cd /d "%~dp0"
where pwsh >nul 2>nul
if errorlevel 1 (
  echo pwsh not found.  Install PowerShell 7, or run:
  echo     ac5.exe --data . --disc "path\to\disc.iso" --watchdog 0
  pause
  exit /b 1
)
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" %*
if errorlevel 1 pause
"@
    Set-Content (Join-Path $dest 'launch.cmd') $cmd -Encoding ASCII
    Write-Host "   + launch.cmd"

    # --- report ------------------------------------------------------------
    $size = (Get-ChildItem $dest -Recurse -File | Measure-Object -Property Length -Sum).Sum
    Write-Host ""
    Write-Host "   $dest" -ForegroundColor Green
    Write-Host ("   {0:N1} MB, {1} files" -f ($size/1MB), (Get-ChildItem $dest -Recurse -File | Measure-Object).Count)
    return $dest
}

$results = @()
foreach ($r in $(if ($Region -eq 'both') { 'cnjp', 'us' } else { $Region })) {
    $results += Export-One $r $Arch
}

Write-Host ""
Write-Host "To use it: copy the folder anywhere on a Windows machine, put your disc" -ForegroundColor Cyan
Write-Host "image wherever you like, and double-click launch.cmd." -ForegroundColor Cyan
Write-Host ""
Write-Host "This package contains ps2_image.bin, which is game code.  Keep it to" -ForegroundColor Yellow
Write-Host "yourself; do not upload it." -ForegroundColor Yellow
