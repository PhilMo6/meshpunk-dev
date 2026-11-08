# Build the Jet 3D engine (Jet rasteriser + private Lua 5.5) as a loadable ELF
# module for ESP32-S3 T-Deck. Uses the Xtensa toolchain from PlatformIO.
#
# -Arch s3 (default) = the Xtensa artifact jet3d.app.elf, unchanged.
# -Arch rv32 = jet3d.app.rv32.elf for the T-Display P4 (the gameboy module's
# pattern): the riscv32-esp toolchain, the P4 firmware's ISA and float ABI
# (rv32imafc, ilp32f), -mno-relax, objects in obj-rv32/, and no -lgcc: the
# rv32 libgcc.a is not position-independent, so every builtin comes from
# host_exports[] (src/elf_host.cpp) and the UND audit below hard-fails on
# anything else. The loader leaves .iram.text where it was loaded on RISC-V
# (lib/elf_loader/elf_loader.c), so the rv32 build drops the Xtensa section
# flag and the l32r audit; it also leaves out CONFIG_IDF_TARGET_ESP32S3
# (Jet's ESP32-S3 PIE assembly), so Jet compiles its portable C++ paths.
param(
    [ValidateSet("s3", "rv32")]
    [string]$Arch = "s3"
)

if ($Arch -eq "rv32") {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-riscv32-esp\bin"
    $CC      = "$toolchain\riscv32-esp-elf-gcc.exe"
    $CXX     = "$toolchain\riscv32-esp-elf-g++.exe"
    $READELF = "$toolchain\riscv32-esp-elf-readelf.exe"
    $ARCH_CFLAGS = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-mno-relax")
    $ARCH_LDFLAGS = @("-Wl,--no-relax")
    $LIBS = @()
    $OUT = "jet3d.app.rv32.elf"
    $obj_dir = "obj-rv32"
} else {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
    $CC      = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
    $CXX     = "$toolchain\xtensa-esp32s3-elf-g++.exe"
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    $OBJDUMP = "$toolchain\xtensa-esp32s3-elf-objdump.exe"
    $ARCH_CFLAGS = @("-mlongcalls", "-DCONFIG_IDF_TARGET_ESP32S3")
    $ARCH_LDFLAGS = @()
    $LIBS = @("-lgcc")
    $OUT = "jet3d.app.elf"
    $obj_dir = "obj"
}

$JET = "jet-src"
$LUA = "lua-src"

# Shared flags. -I. puts the module's own JetConfig.hpp ahead of anything in
# jet-src, which ships only JetConfig.example.hpp.
#
# CONFIG_IDF_TARGET_ESP32S3 (Xtensa only, $ARCH_CFLAGS) gates Jet's inline
# ESP32-S3 PIE assembly (128-bit EE.VST.128.XP stores) in the clear and
# span-fill paths. It pulls in no ESP-IDF headers. ESP_PLATFORM is
# deliberately NOT defined: that would make Jet include esp_attr.h and
# dsps.h, which an ELF module has no include path for.
$COMMON = $ARCH_CFLAGS + @(
    "-shared", "-fPIC", "-fno-common",
    # Hidden visibility + -Bsymbolic below: internal globals bind directly
    # (RELATIVE relocs, no GOT double-hop). Plain -fno-PIC is rejected by
    # the xtensa BFD linker.
    "-fvisibility=hidden",
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-I.", "-I$JET", "-I$LUA",
    # 32-bit lua_Number/lua_Integer halves every TValue on a 32-bit target.
    # This MUST be identical for the C and C++ halves: it selects the width of
    # lua_Number and lua_Integer, so a mismatch silently breaks the ABI between
    # the interpreter and the bindings.
    "-DLUA_32BITS=1"
)

$CFLAGS = $COMMON + @(
    # Force-included ahead of the Lua sources so its lua_writestring /
    # lua_writeline / lua_writestringerror definitions win over the fwrite(stdout)
    # / fprintf(stderr) defaults in llimits.h, which bypass SLOG_LOCK.
    "-include", "lua_out.h",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable"
)

$CXXFLAGS = $COMMON + @(
    "-std=gnu++17",
    "-fno-rtti", "-fno-exceptions",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-sign-compare"
)

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib"
) + $ARCH_LDFLAGS + @(
    "-Wl,-e,main",
    "-Wl,-Bsymbolic",
    # Keep .rel.dyn uncombined - combreloc trips an elf32-xtensa BFD assert.
    "-Wl,-z,nocombreloc",
    "-Wl,--gc-sections"
)

# Rasteriser, transform pipeline and the Lua VM interpreter loop: -O2.
# Everything else is setup or one-shot work and builds smaller at -Os.
# main_tdeck.cpp is here for swap_band_bytes: it touches every pixel of every
# band on the way to the panel, and -Os leaves the loop rolled.
$hot = @(
    "Renderer.cpp", "Scene.cpp", "Object.cpp", "Camera.cpp", "main_tdeck.cpp",
    "lvm.c", "ltable.c", "lgc.c", "lapi.c", "lobject.c", "lstring.c"
)

# Sources that place code in .iram.text. Such a file MUST be built with
# -mtext-section-literals: the section is relocated into internal SRAM as a
# unit, and l32r is PC-relative and fixed at link time, so every literal the
# moved code loads has to sit inside the moved section. The post-link audit
# below fails the build if any l32r target escapes. Empty = no hot section.
# Renderer.cpp: drawTriangle (~12KB, the frame's dominant cost) carries the
# JET_HOT section attribute — internal SRAM fetch on BOTH cores, no icache.
$literals_in_text = @("Renderer.cpp", "Scene.cpp")

$cpp_sources = @()
$cpp_sources += (Get-ChildItem "$JET\*.cpp" | ForEach-Object { $_.FullName })
$cpp_sources += (Get-Item "engine\jet_lua.cpp").FullName
$cpp_sources += (Get-Item "engine\jet_overlay.cpp").FullName
$cpp_sources += (Get-Item "engine\jet_audio.cpp").FullName
$cpp_sources += (Get-Item "engine\jet_mesh.cpp").FullName
$cpp_sources += (Get-Item "engine\jet_terrain.cpp").FullName
$cpp_sources += (Get-Item "main_tdeck.cpp").FullName
$cpp_sources += (Get-Item "cxxstubs.cpp").FullName
$cpp_sources += (Get-Item "ctors_begin.cpp").FullName
$cpp_sources += (Get-Item "ctors_end.cpp").FullName

$c_sources = @(Get-ChildItem "$LUA\*.c" | ForEach-Object { $_.FullName })

# The OPL music stack (engine/music) is carried-in C from the doom module. It
# is compiled with its own flags: compat/ FIRST on the include path so the
# Doom headers those files include resolve to our stand-ins, and WITHOUT the
# forced lua_out.h, which has nothing to do with it.
$music_sources = @(Get-ChildItem "engine\music\*.c" | ForEach-Object { $_.FullName })
$MUSICFLAGS = $COMMON + @(
    "-Iengine\music\compat", "-Iengine\music", "-Iengine",
    "-Wno-unused-variable", "-Wno-unused-but-set-variable"
)

if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime across the tree. Headers are shared, so a single header
# edit can affect any object; if ANY header is newer than an object, that
# object is stale and recompiles.
$headers = @()
$headers += Get-ChildItem -Path $JET -Include *.hpp,*.h -Recurse -ErrorAction SilentlyContinue
$headers += Get-ChildItem -Path $LUA -Include *.h -Recurse -ErrorAction SilentlyContinue
$headers += Get-ChildItem -Path "engine" -Include *.h -Recurse -ErrorAction SilentlyContinue
$headers += Get-ChildItem -Path "." -Filter "JetConfig.hpp" -ErrorAction SilentlyContinue
$newest_header = if ($headers) {
    ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum
} else { [DateTime]::MinValue }

$objects = @()
$failed = $false

# NOTE: the override parameter must not be named $cflags. PowerShell
# variable names are CASE-INSENSITIVE, so $cflags and the script's $CFLAGS
# are the same variable inside this function -- naming it that way silently
# compiled every non-music file with no flags at all, which links to
# "dangerous relocation" errors that look nothing like the real cause.
function Compile-One($src, $isCpp, $flagOverride = $null) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj  = "$obj_dir/$name.o"
    $script:objects += $obj

    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        return
    }

    $srcname = [System.IO.Path]::GetFileName($src)
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
    $extra = if ($Arch -eq "s3" -and $literals_in_text -contains $srcname) { @("-mtext-section-literals") } else { @() }
    Write-Host "  $(if ($isCpp) { 'CXX' } else { 'CC ' }) $srcname ($opt)"
    if ($isCpp) {
        & $CXX $CXXFLAGS $opt $extra -c -o $obj $src
    } else {
        $use = if ($flagOverride) { $flagOverride } else { $CFLAGS }
        & $CC $use $opt $extra -c -o $obj $src
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $srcname"
        $script:failed = $true
    }
}

foreach ($src in $c_sources)     { Compile-One $src $false }
foreach ($src in $music_sources) { Compile-One $src $false $MUSICFLAGS }
foreach ($src in $cpp_sources)   { Compile-One $src $true  }

if ($failed) {
    Write-Host "Compilation failed!"
    exit 1
}

# No -lstdc++: cxxstubs.cpp supplies operator new/delete, the __cxa_* ABI glue
# and the std::__throw_* entry points std::vector references. Linking the real
# library instead drags in its exception and unwind objects (eh_alloc.o,
# unwind-dw2-xtensa.o), whose .rodata carries absolute relocations that a
# -shared link rejects as "dangerous relocation in read-only section".
# Link order carries meaning: ld emits input .ctors sections in the order the
# objects appear, so the begin/end marker objects must bracket every other
# object for run_static_ctors() to find the real constructors between them.
$ordered = @("$obj_dir/ctors_begin.o")
$ordered += ($objects | Where-Object { $_ -notmatch 'ctors_(begin|end)\.o$' })
$ordered += "$obj_dir/ctors_end.o"

Write-Host "Linking..."
& $CXX $CXXFLAGS $LDFLAGS -o $OUT @ordered @LIBS

if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size/1024, 1)) KB)"

# Static-constructor audit. run_static_ctors() walks both tables, but it can
# only reach .ctors if ctors.ld actually took effect. A .ctors section whose
# bounds symbols are missing means every global constructor is skipped in
# silence, so fail the build instead. Jet's Scene.cpp and engine/jet_lua.cpp
# both have file-scope objects with real constructors — this is not academic.
Write-Host ""
$sections = & $READELF -S -W $OUT
$syms = & $READELF --syms -W $OUT

$ctors = $sections | Select-String '\.ctors\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)'
if ($ctors) {
    $secAddr = [Convert]::ToUInt32($ctors.Matches[0].Groups[1].Value, 16)
    $secSize = [Convert]::ToUInt32($ctors.Matches[0].Groups[2].Value, 16)
    $lo = $syms | Select-String '([0-9a-f]{8})\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+__meshpunk_ctors_begin$'
    $hi = $syms | Select-String '([0-9a-f]{8})\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+__meshpunk_ctors_end_marker$'
    if (-not $lo -or -not $hi) {
        Write-Host "ERROR: .ctors present but the boundary markers are missing."
        Write-Host "       Every static constructor would be skipped at runtime."
        exit 1
    }
    $loA = [Convert]::ToUInt32($lo.Matches[0].Groups[1].Value, 16)
    $hiA = [Convert]::ToUInt32($hi.Matches[0].Groups[1].Value, 16)
    # begin must sit at the very start and end at the very last slot, or the
    # markers are not bracketing the real constructors.
    if ($loA -ne $secAddr -or $hiA -ne ($secAddr + $secSize - 4)) {
        Write-Host "ERROR: .ctors markers do not bracket the section."
        Write-Host ("       section {0:x8}+{1}  begin {2:x8}  end {3:x8}" -f $secAddr, $secSize, $loA, $hiA)
        Write-Host "       Link order is wrong; constructors would be missed."
        exit 1
    }
    $n = ($hiA - $loA) / 4 - 1
    Write-Host ".ctors: $n constructor(s) bracketed by the markers, run by run_static_ctors()"
    if ($n -lt 1) { Write-Host "  (none to run)" }
}

$ia = $sections | Select-String '\.init_array\s+INIT_ARRAY\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)'
if ($Arch -eq "rv32") {
    # riscv32-esp: constructors go to .init_array, and the -shared default
    # script places the two .ctors markers into that output section in link
    # order. They must sit at its first and last slot, or run_static_ctors()
    # misses constructors.
    if (-not $ia) {
        Write-Host "ERROR: no .init_array - the ctors markers did not land."
        exit 1
    }
    $iaAddr = [Convert]::ToUInt32($ia.Matches[0].Groups[1].Value, 16)
    $iaSize = [Convert]::ToUInt32($ia.Matches[0].Groups[2].Value, 16)
    $lo = $syms | Select-String '([0-9a-f]{8})\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+__meshpunk_ctors_begin$'
    $hi = $syms | Select-String '([0-9a-f]{8})\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+__meshpunk_ctors_end_marker$'
    if (-not $lo -or -not $hi) {
        Write-Host "ERROR: .init_array present but the boundary markers are missing."
        exit 1
    }
    $loA = [Convert]::ToUInt32($lo.Matches[0].Groups[1].Value, 16)
    $hiA = [Convert]::ToUInt32($hi.Matches[0].Groups[1].Value, 16)
    if ($loA -ne $iaAddr -or $hiA -ne ($iaAddr + $iaSize - 4)) {
        Write-Host "ERROR: the markers do not bracket .init_array."
        Write-Host ("       section {0:x8}+{1}  begin {2:x8}  end {3:x8}" -f $iaAddr, $iaSize, $loA, $hiA)
        exit 1
    }
    $n = ($hiA - $loA) / 4 - 1
    Write-Host ".init_array: $n constructor(s) bracketed by the markers, run front to back by run_static_ctors()"
} elseif ($ia) {
    $n = [Convert]::ToUInt32($ia.Matches[0].Groups[2].Value, 16) / 4
    Write-Host ".init_array: $n constructor(s), run by run_static_ctors()"
}
if (-not $ctors -and -not $ia) {
    Write-Host "constructors: none in this build"
}

# .iram.text is copied into internal SRAM at load, which changes its distance
# to every other section. Absolute references survive (the loader relocates
# them), but l32r is PC-relative and fixed at link time, so every literal it
# reaches must live inside the section itself. RISC-V: the loader runs the
# section in place (PSRAM), so there is nothing to audit.
$iram = & $READELF -S -W $OUT | Select-String '\.iram\.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)'
if ($iram -and $Arch -eq "rv32") {
    $sec_size = [Convert]::ToUInt32($iram.Matches[0].Groups[2].Value, 16)
    Write-Host ""
    Write-Host ".iram.text: $sec_size bytes, run in place from PSRAM on the P4"
} elseif ($iram) {
    $sec_addr = [Convert]::ToUInt32($iram.Matches[0].Groups[1].Value, 16)
    $sec_size = [Convert]::ToUInt32($iram.Matches[0].Groups[2].Value, 16)
    $sec_end  = $sec_addr + $sec_size
    Write-Host ""
    Write-Host ".iram.text: $sec_size bytes of internal SRAM at load"

    $escapes = 0
    $literals = 0
    foreach ($line in (& $OBJDUMP -d --section=.iram.text $OUT)) {
        if ($line -match 'l32r\s+a\d+,\s*(?:0x)?([0-9a-fA-F]+)') {
            $literals++
            $t = [Convert]::ToUInt32($matches[1], 16)
            if ($t -lt $sec_addr -or $t -ge $sec_end) {
                if ($escapes -lt 5) { Write-Host "  ESCAPES SECTION: $($line.Trim())" }
                $escapes++
            }
        }
    }
    if ($escapes -gt 0) {
        Write-Host "  $escapes of $literals l32r targets fall outside .iram.text."
        Write-Host "  The section is not self-contained and will crash once moved."
        exit 1
    }
    Write-Host "  all $literals l32r targets resolve inside the section"
}

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
Write-Host "Undefined symbols (each must be a host export; WEAK ones the loader"
Write-Host "resolves to NULL when unexported, lib/elf_loader/elf_loader.c):"
& $READELF -W --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    $weak = ($parts.Length -ge 8 -and $parts[4] -eq "WEAK")
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") {
        if ($weak -and -not $exported.ContainsKey($sym)) {
            Write-Host "  $sym   (weak: NULL at load)"
        } elseif ($tbl.Success -and -not $exported.ContainsKey($sym)) {
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
