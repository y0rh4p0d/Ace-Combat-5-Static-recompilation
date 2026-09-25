# Build a recompiled Ace Combat 5 for Windows x64 and Windows arm64.
#
#   pwsh -File tools/build_cnjp.ps1                  # x64, CN/JP disc
#   pwsh -File tools/build_cnjp.ps1 -Arch arm64      # arm64 cross build
#   pwsh -File tools/build_cnjp.ps1 -Region us       # the US disc
#   pwsh -File tools/build_cnjp.ps1 -Recompile       # force a re-recompile
#
# The recompile step needs one file out of the disc image: the game executable.
# This script extracts it if it is not already in -ElfDir, then runs the
# recompiler with the config for the chosen region and builds the host binary.

[CmdletBinding()]
param(
    # us   -> SLUS_208.51, Ac5 US release
    # cnjp -> SLPS_254.18, Japanese release with the Chinese localisation
    [ValidateSet('us', 'cnjp')]
    [string]$Region = 'cnjp',

    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',

    [string]$BuildType = 'Release',

    # Where the game executable lives / is extracted to.
    [string]$ElfDir = "$PSScriptRoot\..\work\elf",

    # The disc image to take the executable from when it is missing.
    [string]$UsIso = "$PSScriptRoot\..\ac5us.iso",
    [string]$CnjpIso = "$PSScriptRoot\..\ac5cnjp.iso",

    # MSYS2 UCRT64, used for the x64 build.
    [string]$Msys2 = 'C:\msys64\ucrt64',

    # x86_64 clang driver and AArch64 sysroot, used for the arm64 build.
    [string]$HostTools = "$PSScriptRoot\..\work\host-tools",
    [string]$Sysroot = 'C:\msys64\clangarm64',

    [switch]$Recompile,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
Push-Location $root
try {
    $exe = if ($Region -eq 'us') { 'SLUS_208.51' } else { 'SLPS_254.18' }
    $genDir = if ($Region -eq 'us') { 'generated' } else { 'generated-cnjp' }
    $cfgDir = if ($Region -eq 'us') { 'config' } else { 'config/cnjp' }
    $buildDir = "build/$Arch-$Region"

    Write-Host "== ac5 recompiled build: region=$Region arch=$Arch" -ForegroundColor Cyan

    # --- 1. the game executable -------------------------------------------
    New-Item -ItemType Directory -Force -Path $ElfDir | Out-Null
    $elfPath = Join-Path $ElfDir $exe
    if (-not (Test-Path $elfPath)) {
        $iso = if ($Region -eq 'us') { $UsIso } else { $CnjpIso }
        if (-not (Test-Path $iso)) {
            $iso = if ($Region -eq 'us') { $CnjpIso } else { $UsIso }
        }
        if (-not (Test-Path $iso)) {
            throw "Need $exe but neither the ISO nor a copy in $ElfDir is present. " +
                  "Put the disc image next to this repo or drop $exe into $ElfDir."
        }
        Write-Host "-- extracting $exe from $(Split-Path $iso -Leaf)"
        & python "$PSScriptRoot\extract_elf.py" $iso $exe $elfPath
        if ($LASTEXITCODE -ne 0) { throw "could not extract $exe from $iso" }
    }
    Write-Host "-- executable: $elfPath"

    # --- 2. recompile ------------------------------------------------------
    $funcTable = Join-Path $genDir 'ps2_func_table.c'
    if ($Recompile -or -not (Test-Path $funcTable)) {
        Write-Host "-- recompiling guest code into $genDir"
        $env:PYTHONPATH = "$root\tools"
        $args = @(
            '-m', 'ps2recomp', $elfPath, '-o', $genDir,
            '--ida-db',      "$cfgDir/ida_db.json",
            '--ida-seeds',   "$cfgDir/ida_seeds.json",
            '--symbols',     "$cfgDir/sdk_symbols.json",
            '--symbols',     "$cfgDir/manual_symbols.json",
            '--overrides',   "$cfgDir/overrides.json",
            '--hooks',       "$cfgDir/hooks.json",
            '--report',      "$cfgDir/report.json"
        )
        & python @args
        if ($LASTEXITCODE -ne 0) { throw 'recompiler failed' }
    } else {
        Write-Host "-- reusing $genDir (pass -Recompile to redo it)"
    }

    # --- 2b. reverse address map -------------------------------------------
    # Generated from the port's map, so it describes this executable rather than the
    # reference one.  Without it the runtime cannot turn one of this build's addresses
    # back into a reference address, and the tables still written in reference
    # addresses -- the frontend emitter ranges, for instance -- silently stop matching.
    # The US build is the reference build, where nothing moves, so it needs no table
    # and CMake compiles an empty one.
    if ($Region -ne 'us') {
        $mapJson = Join-Path $cfgDir 'addr_map.json'
        if (Test-Path $mapJson) {
            Write-Host "-- generating the reverse address map"
            & python "$PSScriptRoot\gen_revmap.py" --map $mapJson `
                     --out (Join-Path $genDir 'rn_revmap_data.c')
            if ($LASTEXITCODE -ne 0) { throw 'reverse address map generation failed' }
        } else {
            Write-Host "-- WARNING: $mapJson is missing; the build gets an empty" -ForegroundColor Yellow
            Write-Host "   reverse map, so reference-address tables will not match." -ForegroundColor Yellow
        }
    }

    if ($NoBuild) { return }

    # --- 3. build ----------------------------------------------------------
    switch ($Arch) {
        'x64' {
            $env:PATH = "$Msys2\bin;$env:PATH"
            Write-Host "-- configuring x64 build in $buildDir"
            & cmake -S . -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$BuildType" `
                "-DCMAKE_C_COMPILER=$Msys2/bin/gcc.exe" `
                "-DCMAKE_CXX_COMPILER=$Msys2/bin/c++.exe" `
                "-DPS2_GENERATED_DIR=$genDir"
            if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
        }
        'arm64' {
            Write-Host "-- configuring arm64 cross build in $buildDir"
            $toolchain = "$root/cmake/toolchain-arm64.cmake"
            & cmake -S . -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$BuildType" `
                "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
                "-DPS2_HOST_TOOLS=$(($HostTools -replace '\\','/'))" `
                "-DPS2_SYSROOT=$(($Sysroot -replace '\\','/'))" `
                "-DPS2_GENERATED_DIR=$genDir"
            if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
        }
    }

    Write-Host "-- building"
    & cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }

    Write-Host ""
    Write-Host "done: $buildDir/ac5.exe" -ForegroundColor Green
    Write-Host "run it with:"
    Write-Host "  .\$buildDir\ac5.exe --data $genDir --disc <disc.iso> --watchdog 0"
}
finally {
    Pop-Location
}
