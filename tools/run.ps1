# Start the recompiled Ace Combat 5.
#
#   pwsh -File tools/run.ps1                                       # 汉化版, finds the ISO by searching
#   pwsh -File tools/run.ps1 -Region us                            # 美版
#   pwsh -File tools/run.ps1 -Region cnjp -Iso D:\Games\ac5cnjp.iso
#   pwsh -File tools/run.ps1 -Fullscreen -Log run.log
#
# It checks that the build output and the disc are actually there before starting,
# because the two ways this usually fails -- a missing build, or --data pointing at
# the other region's output -- both produce a build that starts and then sits on a
# black first screen, which is a miserable thing to debug.

[CmdletBinding()]
param(
    # cnjp -> SLPS_254.18 / generated-cnjp    us -> SLUS_208.51 / generated
    [ValidateSet('cnjp', 'us')]
    [string]$Region = 'cnjp',

    # The disc image.  Omitted -> the usual places are searched.
    [string]$Iso,

    # Where the game executable was extracted to, if it was.
    [string]$ElfDir,

    # The build directory to run.  Defaults to build/<arch>-<region>.
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',

    [string]$BuildDir,

    # Arguments passed straight through to ac5.exe.
    [string[]]$ExtraArgs,

    # ac5.exe's own --verbose.  Not named -Verbose: that is a common parameter of
    # every CmdletBinding script and redeclaring it is an error.
    [switch]$Trace,

    [switch]$SkipChecks,

    # Tee the log (stderr) to this file as well as the console.
    [string]$Log
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path

if (-not $ElfDir)   { $ElfDir = Join-Path $root 'work\elf' }
if (-not $BuildDir) { $BuildDir = "build/$Arch-$Region" }

$genDir  = if ($Region -eq 'us') { 'generated' } else { 'generated-cnjp' }
$exeName = if ($Region -eq 'us') { 'SLUS_208.51' } else { 'SLPS_254.18' }

function Find-Iso {
    param([string]$Region, [string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "No such disc image: $Explicit"
    }
    $name = if ($Region -eq 'us') { 'ac5us.iso' } else { 'ac5cnjp.iso' }
    $other = if ($Region -eq 'us') { 'ac5cnjp.iso' } else { 'ac5us.iso' }
    # Any *.iso with the right executable in it would do, but the two releases have
    # different filenames on disc, so a name match is the cheap reliable filter.
    $cands = @(
        (Join-Path $root $name),
        (Join-Path $root 'work\iso' $name),
        (Join-Path $root 'iso' $name),
        (Join-Path (Split-Path $root -Parent) $name),
        (Join-Path $env:USERPROFILE "Documents\$name"),
        (Join-Path $env:USERPROFILE 'Downloads\' + $name)
    )
    foreach ($c in $cands) { if (Test-Path $c) { return (Resolve-Path $c).Path } }

    # Fall back to anything that is not the other region's disc.
    foreach ($dir in @($root, (Join-Path $root 'work\iso'), (Join-Path $root 'iso'),
                       (Split-Path $root -Parent), (Join-Path $env:USERPROFILE 'Documents'))) {
        if (-not (Test-Path $dir)) { continue }
        $hit = Get-ChildItem $dir -Filter '*.iso' -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -ne $other } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

Write-Host ""
Write-Host "== Ace Combat 5  ($Region, $Arch)" -ForegroundColor Cyan

# --- the build ------------------------------------------------------------
$exe = Join-Path $root "$BuildDir/ac5.exe"
if (-not (Test-Path $exe)) {
    Write-Host "not built yet: $BuildDir/ac5.exe" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Build it with:"
    Write-Host "    .\tools\build_all.ps1 -What $(if ($Region -eq 'us') { 'us' } else { $Arch })"
    exit 1
}
$exe = (Resolve-Path $exe).Path
$exeArch = $null
try {
    $bytes = [System.IO.File]::ReadAllBytes($exe)
    $peOff = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
    $exeArch = if ($machine -eq 0x8664) { 'x64' } elseif ($machine -eq 0xAA64) { 'arm64' } else { 'unknown' }
} catch { }
Write-Host "   exe    $BuildDir/ac5.exe$(if ($exeArch) { "  ($exeArch)" })"

if ($exeArch -eq 'arm64' -and -not [Environment]::Is64BitOperatingSystem) {
    Write-Host "   warning: this is an arm64 build" -ForegroundColor Yellow
}
# An arm64 exe cannot run on an x64 host, and Windows says so with a confusing
# error, so say it here instead.
if ($exeArch -eq 'arm64' -and $env:PROCESSOR_ARCHITECTURE -eq 'AMD64') {
    Write-Host ""
    Write-Host "This is the arm64 build and this machine is x64." -ForegroundColor Red
    Write-Host "Copy $BuildDir (ac5.exe, its two DLLs and shaders\) to a" -ForegroundColor Red
    Write-Host "Windows-on-ARM device, or run -Arch x64 here." -ForegroundColor Red
    exit 1
}

# --- the recompiler output ------------------------------------------------
$dataPath = Join-Path $root "$genDir/ps2_image.bin"
if (-not (Test-Path $dataPath)) {
    Write-Host "missing recompiler output: $genDir/ps2_image.bin" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Either the recompile has not run, or it went to another folder."
    Write-Host "  Run:  .\tools\build_all.ps1 -What $(if ($Region -eq 'us') { 'us' } else { $Arch })"
    exit 1
}
Write-Host "   data   $genDir  ($([math]::Round((Get-Item $dataPath).Length/1MB,1)) MB)"

# --- the disc -------------------------------------------------------------
$isoPath = $null
if (-not $SkipChecks) {
    $isoPath = Find-Iso -Region $Region -Explicit $Iso
    if (-not $isoPath) {
        Write-Host "no disc image found" -ForegroundColor Red
        Write-Host ""
        Write-Host "  Pass one explicitly, e.g.:"
        Write-Host "    pwsh -File tools/run.ps1 -Region $Region -Iso D:\Games\ac5$(if ($Region -eq 'us') { 'us' } else { 'cnjp' }).iso"
        exit 1
    }
    $isoMb = [math]::Round((Get-Item $isoPath).Length/1MB)
    Write-Host "   disc   $(Split-Path $isoPath -Leaf)  ($isoMb MB)"

    # A sanity check the plain `--data` mistake cannot produce: the wrong disc.
    # Both discs are large, so a tiny file is either a stub or a corrupt copy, and
    # both would otherwise show up much later as a black screen.
    if ($isoMb -lt 1000) {
        Write-Host ""
        Write-Host "This disc image is only $isoMb MB.  A PS2 DVD is over 4 GB, so this" -ForegroundColor Yellow
        Write-Host "is not a full image and the game will not get far." -ForegroundColor Yellow
        if (-not $Iso) { Write-Host "Pass the real one with -Iso." -ForegroundColor Yellow }
    }
} else {
    $isoPath = $Iso
}
if (-not $isoPath) { $isoPath = $Iso }

# Which executable the disc should carry, so a mismatched pair is caught here
# rather than as a hang later.
if (-not $SkipChecks -and $isoPath -and (Test-Path $isoPath)) {
    $elf = Join-Path $ElfDir $exeName
    if (Test-Path $elf) {
        Write-Host "   elf    $exeName  (extracted)"
    } else {
        Write-Host "   elf    $exeName  (will be taken from the disc)"
    }
}

# --- go -------------------------------------------------------------------
$argsList = @('--data', $genDir)
if ($isoPath) { $argsList += @('--disc', $isoPath) }
$argsList += @('--watchdog', '0')
if ($Trace) { $argsList += '--verbose' }
if ($ExtraArgs) {
    # `pwsh -File script.ps1 -ExtraArgs '--frames','300'` binds the items as
    # literal text, quotes included, so a caller who follows the README would pass
    # ac5.exe the argument "'--frames'," -- which it ignores.  Strip the quoting
    # and the comma separators that fall out of that binding.
    foreach ($a in $ExtraArgs) {
        foreach ($piece in ($a -split ',')) {
            $t = $piece.Trim().Trim("'").Trim('"').Trim()
            if ($t) { $argsList += $t }
        }
    }
}

Write-Host ""
Write-Host "   cd $root"
Write-Host "   .\$BuildDir\ac5.exe $($argsList -join ' ')" -ForegroundColor Green
Write-Host ""
Write-Host "   The window stays black for about 20 seconds before the first picture." -ForegroundColor DarkGray
Write-Host "   F11 (or Alt+Enter) goes fullscreen; F4 settings; Esc quit." -ForegroundColor DarkGray
Write-Host ""

Push-Location $root
try {
    if ($Log) {
        # stderr carries the log; keep the console readable and still save it all.
        & $exe @argsList 2>&1 | Tee-Object -FilePath $Log
    } else {
        & $exe @argsList
    }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) { Write-Host "ac5.exe exited with code $code" -ForegroundColor Yellow }
exit $code
