# One-shot build for the recompiled Ace Combat 5.
#
#   pwsh -File tools/build_all.ps1                 # everything: CN/JP x64, US x64, CN/JP arm64
#   pwsh -File tools/build_all.ps1 -What cnjp      # just the Japanese / Chinese x64 build
#   pwsh -File tools/build_all.ps1 -What arm64     # just the arm64 cross build
#   pwsh -File tools/build_all.ps1 -Recompile      # redo the recompiler step as well
#   pwsh -File tools/build_all.ps1 -Why            # report what is missing, then stop
#
# This wraps the whole chain, including the parts that are easy to get wrong by
# hand: extracting the game executable out of the disc, translating the config when
# the Japanese executable has moved, fetching the arm64 cross toolchain, and
# passing CMake the Vulkan paths that a MinGW/clang toolchain cannot take from the
# LunarG SDK.
#
# Nothing here needs the network except the optional arm64 toolchain download.

[CmdletBinding()]
param(
    # all | cnjp | us | arm64  (arm64 means the Japanese / Chinese arm64 build)
    [ValidateSet('all', 'cnjp', 'us', 'arm64')]
    [string]$What = 'all',

    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$BuildType = 'Release',

    # Redo the recompiler step even when output is already present.
    [switch]$Recompile,

    # Stop after the checks, so you can see what is missing without waiting.
    [switch]$Why,

    # Where the game executables live / are extracted to.
    [string]$ElfDir,

    # Disc images to take the executables from.
    [string]$UsIso,
    [string]$CnjpIso,

    # MSYS2 UCRT64 (x64 gcc + SDL3) and the arm64 cross pieces.
    [string]$Msys2 = 'C:\msys64\ucrt64',
    [string]$Sysroot = 'C:\msys64\clangarm64',
    [string]$HostTools,

    [switch]$KeepGoing   # keep building other targets after one fails
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path

if (-not $ElfDir)   { $ElfDir   = Join-Path $root 'work\elf' }
if (-not $UsIso)    { $UsIso    = Join-Path $root 'ac5us.iso' }
if (-not $CnjpIso)  { $CnjpIso  = Join-Path $root 'ac5cnjp.iso' }
if (-not $HostTools) {
    # Prefer a toolchain that is already there: inside the repo, or in the sibling
    # work/ folder where the setup script puts it when the repo lives in a
    # workspace alongside other projects.
    foreach ($cand in @((Join-Path $root 'work\host-tools'),
                        (Join-Path (Split-Path $root -Parent) 'work\host-tools'))) {
        if (Test-Path (Join-Path $cand 'bin\clang.exe')) { $HostTools = $cand; break }
    }
    if (-not $HostTools) { $HostTools = Join-Path $root 'work\host-tools' }
}

$script:problems = @()
$script:warnings = @()

function Say    { param($m) Write-Host $m }
function Step   { param($m) Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Ok     { param($m) Write-Host "   ok    $m" -ForegroundColor Green }
function Warn   { param($m) Write-Host "   warn  $m" -ForegroundColor Yellow; $script:warnings += $m }
function Bad    { param($m) Write-Host "   MISSING  $m" -ForegroundColor Red; $script:problems += $m }

function Have([string]$name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }

# A program that must be found, optionally at a specific path.
function Need-Path([string]$path, [string]$what) {
    if (Test-Path $path) { Ok $what; return $true }
    Bad "$what (looked for $path)"
    return $false
}

# --- checks ---------------------------------------------------------------
Step 'checking what the build needs'

$python = $null
foreach ($n in 'python', 'python3', 'py') {
    if (Have $n) { $python = $n; break }
}
if ($python) { Ok "python ($python)" } else { Bad 'python 3 on PATH' }

if (Have cmake) {
    $v = (& cmake --version | Select-Object -First 1)
    Ok "cmake ($v)"
} else { Bad 'cmake on PATH' }

# Ninja is what the documented configure lines use; make is not supported here.
if (Have ninja) { Ok 'ninja' } else { Bad 'ninja on PATH  (pip install ninja)' }

$gccOk = Test-Path "$Msys2\bin\gcc.exe"
if ($gccOk) { Ok "msys2 ucrt64 gcc ($Msys2)" }
else { Bad "msys2 ucrt64 toolchain (looked for $Msys2\bin\gcc.exe)" }

# The SDK supplies glslc, which compiles the shaders at build time.
$glslc = (Get-Command glslc -ErrorAction SilentlyContinue).Source
if (-not $glslc -and $env:VULKAN_SDK) {
    $cand = Join-Path $env:VULKAN_SDK 'Bin\glslc.exe'
    if (Test-Path $cand) { $glslc = $cand }
}
if ($glslc) { Ok "glslc ($glslc)" }
else { Bad 'glslc  (ships with the Vulkan SDK; new terminals pick up VULKAN_SDK)' }

# Per-target extras.
$wantUs    = $What -in 'all', 'us'
$wantCnjp  = $What -in 'all', 'cnjp'
$wantArm   = $What -in 'all', 'arm64'

if ($wantArm) {
    if (Test-Path "$HostTools\bin\clang.exe") { Ok "arm64 host clang ($HostTools)" }
    elseif ($Why) {
        Bad "arm64 cross toolchain (no clang in $HostTools; rerun without -Why to fetch it)"
    } else {
        Warn "no x86_64 clang driver in $HostTools"
        Say  "         fetching it with tools/setup_arm64_toolchain.py ..."
        if ($python) {
            & $python (Join-Path $PSScriptRoot 'setup_arm64_toolchain.py') --host-tools $HostTools --sysroot $Sysroot
            if ($LASTEXITCODE -ne 0) { Bad 'arm64 cross toolchain (download failed)' }
            elseif (Test-Path "$HostTools\bin\clang.exe") { Ok 'arm64 cross toolchain (fetched)' }
            else { Bad 'arm64 cross toolchain' }
        }
    }
    Need-Path "$Sysroot\lib\libvulkan-1.dll.a" 'arm64 sysroot vulkan import library' | Out-Null
    Need-Path "$Sysroot\include\vulkan\vulkan.h" 'arm64 sysroot vulkan headers' | Out-Null
    Need-Path "$Sysroot\lib\pkgconfig\sdl3.pc"  'arm64 sysroot SDL3'            | Out-Null
}

# The executables: either already extracted, or reachable through a disc image.
Step 'checking for the game executables'
New-Item -ItemType Directory -Force -Path $ElfDir | Out-Null

function Ensure-Elf([string]$region) {
    $exe = if ($region -eq 'us') { 'SLUS_208.51' } else { 'SLPS_254.18' }
    $path = Join-Path $ElfDir $exe
    if (Test-Path $path) { Ok "$exe (already in $ElfDir)"; return $path }

    $iso = if ($region -eq 'us') { $UsIso } else { $CnjpIso }
    $alt = if ($region -eq 'us') { $CnjpIso } else { $UsIso }
    if (-not (Test-Path $iso)) { $iso = $alt }
    if (-not (Test-Path $iso)) {
        Bad "$exe  (no copy in $ElfDir and no disc image to take it from)"
        return $null
    }
    Ok "$exe (will extract from $(Split-Path $iso -Leaf))"
    return @{ extract = $iso; dest = $path; name = $exe }
}

$elfUs = $null; $elfCnjp = $null
if ($wantUs)   { $elfUs   = Ensure-Elf 'us' }
if ($wantCnjp -or $wantArm) { $elfCnjp = Ensure-Elf 'cnjp' }

if ($Why) {
    Say ''
    if ($script:problems.Count) {
        Say "missing: $($script:problems.Count)" -ForegroundColor Red
        $script:problems | ForEach-Object { Say "  - $_" }
        exit 1
    }
    Say 'everything needed is present.' -ForegroundColor Green
    exit 0
}

if ($script:problems.Count) {
    Say ''
    Say "cannot build, missing:" -ForegroundColor Red
    $script:problems | ForEach-Object { Say "  - $_" }
    Say ''
    Say 'See "What you need" in README.md.'
    exit 1
}

if (-not $python) { throw 'python is required' }

# --- helpers --------------------------------------------------------------
function Extract-Elf($spec) {
    if ($spec -isnot [hashtable]) { return $spec }
    Write-Host "-- extracting $($spec.name) from $(Split-Path $spec.extract -Leaf)"
    # Pipe to Write-Host, not just to the console: a bare `& tool` in a function
    # puts the tool's stdout into the function's return value, so the caller would
    # get "extracted ... -> path" glued to the path it actually wants.
    & $python (Join-Path $PSScriptRoot 'extract_elf.py') $spec.extract $spec.name $spec.dest |
        Write-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $spec.dest)) {
        throw "could not extract $($spec.name)"
    }
    return $spec.dest
}

# Run the recompiler for one region, unless its output is already there.
# The reverse address map, generated from the port's map for this region.
#
# It describes this executable, so it is per-region data and is rebuilt even when the
# recompiler output is reused: a build that quietly lacks it still runs, but the
# renderer can no longer turn one of this build's addresses back into a reference
# address, and every table still written in reference addresses stops matching -- with
# nothing logged, because a failed comparison and a failed range test say nothing.
function Update-ReverseMap([string]$region, [string]$genDir) {
    if ($region -eq 'us') {
        # The US build is the reference build: nothing moves, so it needs no table and
        # CMake compiles an empty one.  Generating one here would be actively wrong.
        return
    }
    $cfgDir = 'config/cnjp'
    $map = Join-Path $root "$cfgDir\addr_map.json"
    if (-not (Test-Path $map)) {
        Write-Host "-- WARNING: $cfgDir\addr_map.json is missing." -ForegroundColor Yellow
        Write-Host "   The reverse address map cannot be generated, so the build gets an" -ForegroundColor Yellow
        Write-Host "   empty one and the renderer will not recognise its reference-address" -ForegroundColor Yellow
        Write-Host "   tables.  Regenerate it with the map step in tools/cnjp/." -ForegroundColor Yellow
        return
    }
    Write-Host "-- generating the reverse address map into $genDir"
    & $python (Join-Path $root 'tools\gen_revmap.py') --map $map `
             --out (Join-Path $root "$genDir\rn_revmap_data.c") | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "reverse address map generation failed for $region" }
}

function Recompile-Guest([string]$region, [string]$elf) {
    $genDir = if ($region -eq 'us') { 'generated' } else { 'generated-cnjp' }
    $cfgDir = if ($region -eq 'us') { 'config' } else { 'config/cnjp' }

    if (-not $Recompile -and (Test-Path (Join-Path $root "$genDir\ps2_func_table.c"))) {
        Write-Host "-- reusing $genDir  (pass -Recompile to redo it)"
        Update-ReverseMap $region $genDir
        return $genDir
    }
    Write-Host "-- recompiling $region guest code into $genDir"
    $env:PYTHONPATH = Join-Path $root 'tools'
    $a = @(
        '-m', 'ps2recomp', $elf, '-o', $genDir,
        '--ida-db',    "$cfgDir/ida_db.json",
        '--ida-seeds', "$cfgDir/ida_seeds.json",
        '--symbols',   "$cfgDir/sdk_symbols.json",
        '--symbols',   "$cfgDir/manual_symbols.json",
        '--overrides', "$cfgDir/overrides.json",
        '--hooks',     "$cfgDir/hooks.json",
        '--report',    "$cfgDir/report.json"
    )
    & $python @a | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "recompiler failed for $region" }
    Update-ReverseMap $region $genDir
    return $genDir
}

function Configure-Build([string]$arch, [string]$region, [string]$genDir) {
    $buildDir = "build/$arch-$region"
    Write-Host "-- configuring $buildDir"

    if ($arch -eq 'x64') {
        $env:PATH = "$Msys2\bin;$env:PATH"
        & cmake -S $root -B (Join-Path $root $buildDir) -G Ninja `
            "-DCMAKE_BUILD_TYPE=$BuildType" `
            "-DCMAKE_C_COMPILER=$(($Msys2 -replace '\\','/'))/bin/gcc.exe" `
            "-DCMAKE_CXX_COMPILER=$(($Msys2 -replace '\\','/'))/bin/c++.exe" `
            "-DPS2_GENERATED_DIR=$genDir" | Write-Host
    } else {
        $tc = Join-Path $root 'cmake/toolchain-arm64.cmake'
        # The Vulkan SDK ships MSVC import libraries, which a MinGW/clang
        # toolchain cannot consume, so point CMake at the sysroot's own copies.
        $vk = "$(($Sysroot -replace '\\','/'))/lib/libvulkan-1.dll.a"
        $vkInc = "$(($Sysroot -replace '\\','/'))/include"
        & cmake -S $root -B (Join-Path $root $buildDir) -G Ninja `
            "-DCMAKE_BUILD_TYPE=$BuildType" `
            "-DCMAKE_TOOLCHAIN_FILE=$tc" `
            "-DPS2_HOST_TOOLS=$(($HostTools -replace '\\','/'))" `
            "-DPS2_SYSROOT=$(($Sysroot -replace '\\','/'))" `
            "-DPS2_GENERATED_DIR=$genDir" `
            "-DVulkan_LIBRARY=$vk" `
            "-DVulkan_INCLUDE_DIR=$vkInc" | Write-Host
    }
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $buildDir" }
    return $buildDir
}

function Build-One([string]$arch, [string]$region, $elfSpec) {
    Step "building $arch-$region"
    $elf = Extract-Elf $elfSpec
    if (-not $elf) { throw "no executable for $region" }
    $genDir = Recompile-Guest $region $elf
    $buildDir = Configure-Build $arch $region $genDir

    Write-Host "-- compiling (this takes a few minutes)"
    & cmake --build (Join-Path $root $buildDir) | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "build failed for $buildDir" }

    $out = Join-Path $root "$buildDir/ac5.exe"
    $mb = [math]::Round((Get-Item $out).Length / 1MB)
    Ok "$buildDir/ac5.exe  ($mb MB, $arch)"
    return @{ dir = $buildDir; gen = $genDir; region = $region }
}

# --- build ----------------------------------------------------------------
$results = @()
$targets = @()
if ($wantCnjp) { $targets += @{ arch = 'x64';   region = 'cnjp'; elf = $elfCnjp; disc = $CnjpIso } }
if ($wantUs)   { $targets += @{ arch = 'x64';   region = 'us';   elf = $elfUs;   disc = $UsIso } }
if ($wantArm)  { $targets += @{ arch = 'arm64'; region = 'cnjp'; elf = $elfCnjp; disc = $CnjpIso } }

$failed = @()
foreach ($t in $targets) {
    try {
        $r = Build-One $t.arch $t.region $t.elf
        $r.disc = $t.disc
        $results += $r
    } catch {
        Write-Host "   FAILED  $($t.arch)-$($t.region): $_" -ForegroundColor Red
        $failed += "$($t.arch)-$($t.region)"
        if (-not $KeepGoing) { throw }
    }
}

# --- summary --------------------------------------------------------------
Step 'done'
foreach ($r in $results) {
    $disc = if ($r.disc) { Split-Path $r.disc -Leaf } else { '<disc.iso>' }
    Write-Host ""
    Write-Host "  $($r.dir)" -ForegroundColor Green
    Write-Host "    .\$($r.dir)\ac5.exe --data $($r.gen) --disc $disc --watchdog 0"
}
if ($failed.Count) {
    Write-Host ""
    Write-Host "  failed: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host ""
Write-Host "  The arm64 build cannot be run on an x64 machine; copy the whole folder" -ForegroundColor DarkGray
Write-Host "  (ac5.exe, its two DLLs and shaders\) to a Windows-on-ARM device." -ForegroundColor DarkGray
