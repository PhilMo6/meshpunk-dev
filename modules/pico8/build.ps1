# Build fake-08 (PICO-8) as a loadable ELF module for ESP32-S3 T-Deck
# Uses the Xtensa toolchain from PlatformIO
# The ELF stays in this folder: PICO-8 ships through the App Library
# (meshpunk-apps-dev/apps/PICO-8, then the stable catalog through that repo's
# promote script), not the firmware data tree.
#
# -Arch s3 (default) = the Xtensa artifact pico8.app.elf, unchanged.
# -Arch rv32 = pico8.app.rv32.elf for the T-Display P4 (the gameboy module's
# pattern): the riscv32-esp toolchain, the P4 firmware's ISA and float ABI
# (rv32imafc, ilp32f), -mno-relax, objects in obj-rv32/, and no libraries:
# the rv32 libgcc.a and libstdc++.a are not position-independent, so every
# import comes from host_exports[] (src/elf_host.cpp) and the UND audit below
# hard-fails on anything else.
param(
    [ValidateSet("s3", "rv32")]
    [string]$Arch = "s3"
)

# Ensure all relative paths resolve from the script's own directory,
# regardless of where it's invoked from (e.g. project root).
Push-Location $PSScriptRoot
trap { Pop-Location }

if ($Arch -eq "rv32") {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-riscv32-esp\bin"
    $CXX = "$toolchain\riscv32-esp-elf-g++.exe"
    $SIZE = "$toolchain\riscv32-esp-elf-size.exe"
    $READELF = "$toolchain\riscv32-esp-elf-readelf.exe"
    $ARCH_FLAGS = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-mno-relax")
    $ARCH_LDFLAGS = @("-Wl,--no-relax")
    $OUT = "pico8.app.rv32.elf"
    $obj_dir = "obj-rv32"
} else {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
    $CXX = "$toolchain\xtensa-esp32s3-elf-g++.exe"
    $SIZE = "$toolchain\xtensa-esp32s3-elf-size.exe"
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    $ARCH_FLAGS = @("-mlongcalls")
    $ARCH_LDFLAGS = @(
        "-lgcc",
        "-L$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\xtensa-esp32s3-elf\lib\no-rtti",
        "-lstdc++",
        "-lgcc"
    )
    $OUT = "pico8.app.elf"
    $obj_dir = "obj"
}

$F08 = "fake08-src"

# Common compiler flags
# -fwrapv / -fno-strict-aliasing are correctness flags, not tuning: z8lua
# emulates PICO-8's 16.16 fixed-point with int32 arithmetic whose wraparound
# is the spec (signed overflow is UB the optimizer exploits at -O2 — seen as
# healthy carts throwing nil-field/type errors mid-game), and it's C type-
# punning code compiled as C++.
$COMMON_FLAGS = @(
    "-shared", "-fPIC", "-fno-common",
    "-Os"
) + $ARCH_FLAGS + @(
    "-fwrapv", "-fno-strict-aliasing",
    "-ffunction-sections", "-fdata-sections",
    "-D_TDECK",
    "-I$F08/source",
    "-I$F08/libs/z8lua",
    "-I$F08/libs/lodepng",
    "-I$F08/libs/simpleini",
    "-I$F08/libs/miniz"
)

$CXXFLAGS = $COMMON_FLAGS + @(
    "-fno-rtti", "-fno-exceptions",
    "-std=gnu++17",
    "-Wno-write-strings"
)

# z8lua .c files compiled as C++ (luaconf.h includes <cstdint>)
$CFLAGS_Z8LUA = $COMMON_FLAGS + @(
    "-fno-rtti", "-fno-exceptions",
    "-std=gnu++17",
    "-DLUA_USE_LONGJMP",
    "-x", "c++",
    "-Wno-write-strings"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib"
) + $ARCH_LDFLAGS + @(
    "-Wl,-e,main",
    "-Wl,--gc-sections"
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------

# Our T-Deck platform files
$tdeck_sources = @(
    "main_tdeck.cpp",
    "TDeckHost.cpp",
    "cxxstubs.cpp",
    "filehelpers_tdeck.cpp"
)

# fake-08 core sources — exclude files we replace with T-Deck implementations
$F08_EXCLUDE = @("main.cpp", "hostCommonFunctions.cpp", "filehelpers.cpp")
$f08_sources = Get-ChildItem "$F08/source/*.cpp" | Where-Object {
    $F08_EXCLUDE -notcontains $_.Name
} | ForEach-Object { $_.FullName }

# z8lua (C files compiled as C++)
$z8lua_sources = Get-ChildItem "$F08/libs/z8lua/*.c" | ForEach-Object { $_.FullName }

# lodepng
$lodepng_sources = @("$F08/libs/lodepng/lodepng.cpp")

Write-Host "Building PICO-8 module..."
Write-Host "  T-Deck:   $($tdeck_sources.Count) files"
Write-Host "  fake-08:  $($f08_sources.Count) files"
Write-Host "  z8lua:    $($z8lua_sources.Count) files"
Write-Host "  lodepng:  1 file"

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

$objects = @()
$failed = $false

# Hot-path files compiled -O2 instead of -Os: the audio synth runs per sample
# and the PICO-8 gfx API per draw call. (gcc honors the last -O flag.)
$HOT_O2 = @("Audio", "synth", "filter", "graphics")

# Compile C++ sources (T-Deck platform + fake-08 core + lodepng)
$cpp_sources = $tdeck_sources + $f08_sources + $lodepng_sources
foreach ($src in $cpp_sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    # Only recompile if source is newer than object
    if ((Test-Path $obj) -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $flags = $CXXFLAGS
    if ($HOT_O2 -contains $name) { $flags = $flags + @("-O2") }

    Write-Host "  CXX $name"
    & $CXX $flags -c -o $obj $src
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $name"
        $failed = $true
    }
}

# Compile z8lua .c files as C++ (they use <cstdint> via luaconf.h).
# z8lua runs -O2: the interpreter dominates cart step time (boulder ~37-57
# ms/f at -Os). The historical -O2 cart corruption was optimizer-exploited
# UB (fix32 signed-overflow wraparound + C type-punning as C++), pinned down
# by -fwrapv -fno-strict-aliasing in COMMON_FLAGS — those flags are the
# precondition for -O2 here, never remove them. If nil-field/type errors
# return mid-game, drop back to -Os and report.
foreach ($src in $z8lua_sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    if ((Test-Path $obj) -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    Write-Host "  CXX $name (z8lua -O2)"
    & $CXX $CFLAGS_Z8LUA -O2 -c -o $obj $src
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $name"
        $failed = $true
    }
}

if ($failed) {
    Write-Host "Compilation failed!"
    exit 1
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
Write-Host "Linking..."
& $CXX $COMMON_FLAGS $LDFLAGS -o $OUT @objects

if ($LASTEXITCODE -eq 0) {
    & $SIZE $OUT
    $fileSize = (Get-Item $OUT).Length
    Write-Host "Success: $OUT ($([math]::Round($fileSize/1024, 1)) KB)"
} else {
    Write-Host "Link failed!"
    Pop-Location
    exit 1
}

# Every UND symbol must be resolvable from host_exports[] in src/elf_host.cpp,
# or the module will fail to load on device. The table is parsed from the
# source; a miss is printed for both arches and fails the rv32 build (the
# Xtensa build only prints).
$hostSrc = Get-Content (Join-Path $PSScriptRoot "..\..\src\elf_host.cpp") -Raw
$tbl = [regex]::Match($hostSrc, 'host_exports\[\]\s*=\s*\{(.*?)\n\};', 'Singleline')
$exported = @{}
if ($tbl.Success) {
    foreach ($em in [regex]::Matches($tbl.Groups[1].Value, '\{\s*"([^"]+)"')) {
        $exported[$em.Groups[1].Value] = $true
    }
} else {
    Write-Host "WARNING: could not parse host_exports[] from src/elf_host.cpp"
}
$missing = @()
Write-Host ""
Write-Host "Undefined symbols (each must be a host export):"
& $READELF -W --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") {
        if ($tbl.Success -and -not $exported.ContainsKey($sym)) {
            Write-Host "  $sym   <-- NOT IN host_exports[]"
            $missing += $sym
        } else {
            Write-Host "  $sym"
        }
    }
}
if ($missing.Count -gt 0 -and $Arch -eq "rv32") {
    Write-Host ""
    Write-Host "ERROR: $($missing.Count) symbol(s) missing from host_exports[] - the elf WILL fail to load:"
    $missing | ForEach-Object { Write-Host "  $_" }
    Pop-Location
    exit 1
}

Pop-Location
