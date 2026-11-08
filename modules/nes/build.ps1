# Build Nofrendo (NES emulator) as a loadable ELF module
# for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.
#
# -Arch s3 (default) = the Xtensa artifact nes.app.elf, unchanged.
# -Arch rv32 = nes.app.rv32.elf for the T-Display P4 (the gameboy module's
# pattern): the riscv32-esp toolchain, the P4 firmware's ISA and float ABI
# (rv32imafc, ilp32f), -mno-relax, objects in obj-rv32/, and no -lgcc: the
# rv32 libgcc.a is not position-independent, so every builtin comes from
# host_exports[] (src/elf_host.cpp) and the UND audit below hard-fails on
# anything else.
param(
    [ValidateSet("s3", "rv32")]
    [string]$Arch = "s3"
)

if ($Arch -eq "rv32") {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-riscv32-esp\bin"
    $CC = "$toolchain\riscv32-esp-elf-gcc.exe"
    $READELF = "$toolchain\riscv32-esp-elf-readelf.exe"
    $ARCH_CFLAGS = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-mno-relax")
    $ARCH_LDFLAGS = @("-Wl,--no-relax")
    $LIBS = @()
    $OUT = "nes.app.rv32.elf"
    $obj_dir = "obj-rv32"
} else {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
    $CC = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    $ARCH_CFLAGS = @("-mlongcalls")
    $ARCH_LDFLAGS = @()
    $LIBS = @("-lgcc")
    $OUT = "nes.app.elf"
    $obj_dir = "obj"
}

$NF = "nofrendo-src\src"

# Common compiler flags (per-file -O flag added below)
$CFLAGS = $ARCH_CFLAGS + @(
    "-shared", "-fPIC", "-fno-common",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-I$NF",
    "-I$NF\cpu",
    "-I$NF\nes",
    "-I$NF\sndhrdw",
    "-I$NF\mappers",
    "-I$NF\libsnss",
    "-Wno-implicit-function-declaration",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-pointer-to-int-cast",
    "-Wno-int-conversion"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib"
) + $LIBS + $ARCH_LDFLAGS + @(
    "-Wl,-e,main",
    "-Wl,--gc-sections"
)

# 6502 interpreter, PPU/APU and frame plumbing: -O2. Rest: -Os.
$hot = @(
    "nes6502.c", "nes_ppu.c", "nes_apu.c",
    "nes.c", "vid_drv.c", "bitmap.c"
)

# Nofrendo framework (intro.c and pcx.c are not built — desktop-only)
$core_sources = @(
    "$NF\bitmap.c", "$NF\config.c", "$NF\event.c", "$NF\gui.c",
    "$NF\gui_elem.c", "$NF\log.c", "$NF\memguard.c", "$NF\nofrendo.c",
    "$NF\vid_drv.c"
)
# NES machine, CPU, sound hardware, mappers, save states
$core_sources += Get-ChildItem "$NF\nes\*.c"     | ForEach-Object { $_.FullName }
$core_sources += Get-ChildItem "$NF\cpu\*.c"     | ForEach-Object { $_.FullName }
$core_sources += Get-ChildItem "$NF\sndhrdw\*.c" | ForEach-Object { $_.FullName }
$core_sources += Get-ChildItem "$NF\mappers\*.c" | ForEach-Object { $_.FullName }
$core_sources += Get-ChildItem "$NF\libsnss\*.c" | ForEach-Object { $_.FullName }
# Platform glue
$glue_sources = @("main_tdeck.c", "osd_tdeck.c")

if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime anywhere under the source tree. Headers are shared, so a
# single header edit can affect any object. We do not parse per-file #includes;
# if ANY header is newer than an object, that object is stale and recompiles.
$headers = Get-ChildItem -Path $NF -Filter *.h -Recurse -ErrorAction SilentlyContinue
$newest_header = if ($headers) {
    ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum
} else { [DateTime]::MinValue }

$objects = @()
$failed = $false

foreach ($src in ($core_sources + ($glue_sources | ForEach-Object { (Get-Item $_).FullName }))) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    # Recompile if the source OR any header is newer than the object.
    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
    Write-Host "  CC $srcname ($opt)"
    & $CC $CFLAGS $opt -c -o $obj $src
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $srcname"
        $failed = $true
    }
}

if ($failed) {
    Write-Host "Compilation failed!"
    exit 1
}

Write-Host "Linking..."
& $CC $CFLAGS $LDFLAGS -o $OUT @objects

if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size/1024, 1)) KB)"

# Every UND symbol must be resolvable from host_exports[] in src/elf_host.cpp,
# or the module will fail to load on device. Audit at build time instead:
# the table is parsed from the source; a miss is printed for both arches and
# fails the rv32 build (the Xtensa build keeps its print-only behaviour).
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
    exit 1
}
