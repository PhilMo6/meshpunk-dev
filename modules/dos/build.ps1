# Build tiny386 (386 DOS emulator) as a loadable ELF module for ESP32-S3 T-Deck.
# Uses the Xtensa toolchain from PlatformIO. See tiny386-src/VENDOR.md for the
# vendoring rules and every MESHPUNK-tagged local edit.
#
# -Arch s3 (default) = the Xtensa artifact dos.app.elf, unchanged.
# -Arch rv32 = dos.app.rv32.elf for the T-Display P4 (the gameboy module's
# pattern): the riscv32-esp toolchain, the P4 firmware's ISA and float ABI
# (rv32imafc, ilp32f), -mno-relax, objects in obj-rv32/, and no -lgcc: the
# rv32 libgcc.a is not position-independent, so every builtin comes from
# host_exports[] (src/elf_host.cpp) and the UND audit below hard-fails on
# anything else. The loader leaves .iram.text where it was loaded on RISC-V
# (lib/elf_loader/elf_loader.c), so the rv32 build drops the Xtensa section
# flags and the l32r audit.
param(
    [ValidateSet("s3", "rv32")]
    [string]$Arch = "s3"
)

if ($Arch -eq "rv32") {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-riscv32-esp\bin"
    $CC  = "$toolchain\riscv32-esp-elf-gcc.exe"
    $CXX = "$toolchain\riscv32-esp-elf-g++.exe"
    $READELF = "$toolchain\riscv32-esp-elf-readelf.exe"
    $ARCH_CFLAGS = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-mno-relax")
    $LIBS = @()
    $OUT  = "dos.app.rv32.elf"
    $obj_dir = "obj-rv32"
} else {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
    $CC  = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
    $CXX = "$toolchain\xtensa-esp32s3-elf-g++.exe"
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    $OBJDUMP = "$toolchain\xtensa-esp32s3-elf-objdump.exe"
    $ARCH_CFLAGS = @("-mlongcalls")
    $LIBS = @("-lgcc")
    $OUT  = "dos.app.elf"
    $obj_dir = "obj"
}

$T386 = "tiny386-src"

# BUILD_ESP32 is deliberately NOT defined — it selects ESP-IDF specifics
# (esp_attr.h, IRAM_ATTR on 31 functions, esp_mac.h, a bump-allocator
# pcmalloc) that an ELF module running from PSRAM must not have.
#
# -fvisibility=hidden + -Bsymbolic: module-internal globals bind directly
# (RELATIVE relocs) instead of going through the GOT. Only host imports do.
$COMMON = @(
    "-shared", "-fPIC", "-fvisibility=hidden", "-fno-common"
) + $ARCH_CFLAGS + @(
    "-ffunction-sections", "-fdata-sections",
    "-fno-strict-aliasing",
    "-DBPP=16",                          # native RGB565 framebuffer
    "-Dfseeko=fseek", "-Dftello=ftell",  # host exports no 64-bit *o variants
    "-DI386_ENABLE_FPU",                 # x87
    "-DUSE_FMOPL",                       # Adlib OPL2 music (LGPL, see VENDOR.md)
    # How many guest instructions run before control returns to the main loop.
    # Upstream bounds this so the watchdog gets fed; the reason to RAISE it is
    # cache. Every return runs i8254_update_irq, cmos_update_irq, kbd_step,
    # ne2000_step, vga_step and the input/flush/audio checks -- ~10 calls into
    # PSRAM-resident code, 1300x/s, each dragging its own lines through the same
    # 16KB icache the interpreter is competing for. The direct cost is only ~5%
    # of wall; the eviction cost is invisible in the buckets and lands on `step`.
    # DO NOT RAISE THIS. Tried 1024 (2026-07-27), hw-rejected: interrupts are
    # only delivered BETWEEN pc_step() calls, and the PIC's IRR holds ONE bit
    # per line -- so two IRQ 5s arriving inside one batch COLLAPSE INTO ONE ISR
    # run. That is the guest servicing less audio, i.e. the same trade the audio
    # rate divider makes, not a real speed win. Phil heard it immediately.
    # Any future "batch more instructions" idea has this same flaw.
    "-DPC_STEP_COUNT=512",
    "-DMIXER_BUF_LEN=512",               # mixer_callback runs on the sound task
    "-DTMPBUF_LEN=512",                  # write_audio bounce buffer; upstream's
                                         # own ESP32 value (its 4096 desktop
                                         # default is a 4KB stack frame per call)
    "-DMESHPUNK_NO_TTY",                 # no host console; stub misc.c's tty helpers
    "-include", "dos_mem.h",             # hot-state placement; see the header
    # DIAGNOSTIC (currently OFF). -DDOS_WAVCAP=1 writes every sample handed to
    # the firmware to /sd/dos_audio.wav. SLOW, and it CONTAMINATES what it
    # measures: each push does an SD write with the guest frozen, and those
    # stalls exceed the DMA block period -- creating the exact ack-latency the
    # concealment (and before it, the stale laps) trigger on. Audio judged with
    # this on is audio judged mid-seizure. Enable only to capture, never to listen.
    "-I$T386", "-I.",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-format"
)

# C-only warning switches (cc1plus rejects these).
$CFLAGS   = $COMMON + @("-std=gnu11",
    "-Wno-implicit-function-declaration",
    "-Wno-pointer-to-int-cast",
    "-Wno-int-conversion")
$CXXFLAGS = $COMMON + @("-std=gnu++17", "-fno-rtti", "-fno-exceptions")

$LDFLAGS = @(
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,-e,main",
    "-Wl,-Bsymbolic",
    "-Wl,--gc-sections",
    # BFD elf32-xtensa relaxation is buggy in this binutils. The rv32 build
    # (compiled -mno-relax) links without relaxation as well.
    "-Wl,--no-relax",
    # elf32-xtensa asserts its R_XTENSA_RTLD placeholder relocs are still at
    # the head of .rela.got, but the default -z combreloc sort shuffles them.
    # Our elf_loader walks relocs linearly and ignores order.
    "-Wl,-z,nocombreloc"
)

# Audio synthesis interleaves with the interpreter on Core 0 and shares no
# working set with it, so it evicts interpreter lines from the 16KB instruction
# cache between emulation batches. These files carry the ~3.5KB hot path into
# .iram.text (internal SRAM, fetched outside the cache entirely) -- see
# dos_hot.h for the rules, especially why -mtext-section-literals is mandatory
# rather than optional.
# i386.c joins for the interpreter's per-instruction workhorses -- fetch, modsib,
# load/store, translate, __GE_helper, get_OF. Those are CALLS from cpu_exec1
# (confirmed with objdump -r), never inlined, so tagging them cannot de-inline
# anything; see MESHPUNK_IRAM in i386.c for the exclusion list and why.
# cpu_exec1 itself stays in PSRAM: 98KB, nowhere near fits.
$hot_iram = @("fmopl.c", "sb16.c", "pcspk.c", "adlib.c", "pc.c", "main_tdeck.c",
              "i386.c")
# -mtext-section-literals is an Xtensa option; the P4 runs the section in place.
$IRAM_FLAGS = if ($Arch -eq "rv32") { @("-DMESHPUNK_HOT_IRAM", "-include", "dos_hot.h") }
              else { @("-DMESHPUNK_HOT_IRAM", "-mtext-section-literals", "-include", "dos_hot.h") }

# i386.c is THE hot file (the interpreter dispatch loop) -> -O2.
# Everything else -Os: on this shared-cache silicon these cores are
# memory-bound, and -O3 measured as a null on the NGPC/SNES campaigns.
$hot = @("i386.c")

$core_sources = @(
    "$T386\i386.c", "$T386\pc.c", "$T386\vga.c", "$T386\ide.c",
    "$T386\i8042.c", "$T386\i8254.c", "$T386\i8257.c", "$T386\i8259.c",
    "$T386\sb16.c", "$T386\pcspk.c", "$T386\adlib.c", "$T386\fmopl.c",
    "$T386\fpu.c", "$T386\misc.c", "$T386\pci.c", "$T386\ini.c",
    "$T386\ne2000.c"
)
$glue_c   = @("main_tdeck.c", "dos_video.c", "dos_input.c", "dos_libc_shim.c")
$glue_cpp = @("dos_folderdisk.cpp", "dos_cxxstubs.cpp")

if (-not (Test-Path $obj_dir)) { New-Item -ItemType Directory $obj_dir | Out-Null }

# Newest header mtime anywhere under the source tree. Headers are shared, so a
# single header edit can affect any object.
$headers = Get-ChildItem -Path $T386 -Filter *.h -Recurse -ErrorAction SilentlyContinue
$newest_header = if ($headers) {
    ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum
} else { [DateTime]::MinValue }

$objects = @()
$failed = $false

foreach ($src in ($core_sources + $glue_c + $glue_cpp)) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$obj_dir/$name.o"
    $objects += $obj

    if ((Test-Path $obj) `
        -and ((Get-Item $src).LastWriteTime -le (Get-Item $obj).LastWriteTime) `
        -and ($newest_header -le (Get-Item $obj).LastWriteTime)) {
        continue
    }

    $srcname = [System.IO.Path]::GetFileName($src)
    $isCpp = $srcname.EndsWith(".cpp")
    $opt = if ($hot -contains $srcname) { "-O2" } else { "-Os" }
    $extra = if ($hot_iram -contains $srcname) { $IRAM_FLAGS } else { @() }

    if ($isCpp) {
        Write-Host "  CXX $srcname ($opt)"
        & $CXX $CXXFLAGS $extra $opt -c -o $obj $src
    } else {
        Write-Host "  CC  $srcname ($opt)"
        & $CC $CFLAGS $extra $opt -c -o $obj $src
    }
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
# -lgcc MUST come after the objects: the linker only pulls an archive member
# when something already on the line needs it. With -lgcc first, the 64-bit
# shift/compare helpers (__ashldi3, __floatundidf, ...) stay undefined and
# would have to be resolved by the host at load time instead.
& $CC $COMMON $LDFLAGS -o $OUT @objects $LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}

Write-Host ""
Write-Host "Built $OUT"
Get-Item $OUT | Select-Object Name, Length

# Undefined symbols must all be host exports (see src/elf_host.cpp). The table
# is parsed from the source; a miss is printed for both arches and fails the
# rv32 build (the Xtensa build keeps its print-only behaviour).
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
Write-Host ""
Write-Host "Undefined symbols (must all be host exports):"
$und = & $READELF --dyn-syms -W $OUT | Select-String "\sUND\s" | ForEach-Object {
    ($_.Line.Trim() -split '\s+')[-1]
} | Where-Object { $_ -ne "" -and $_ -ne "UND" } | Sort-Object -Unique
$missing = @()
foreach ($sym in $und) {
    if ($tbl.Success -and -not $exported.ContainsKey($sym)) {
        Write-Host "  $sym   <-- NOT IN host_exports[]"
        $missing += $sym
    } else {
        Write-Host "  $sym"
    }
}

# Our own macros must never reach the linker. If one does, a call site outlived
# its #define and -Wno-implicit-function-declaration turned it into a silent
# implicit call -- which the ELF loader only rejects on the device, at load time.
$ours = $und | Where-Object { $_ -match 'DOSCLIP|DOSPROBE|DOSAUD|DOSPERF|MESHPUNK|dosclip_|dosprobe_|dosperf_' }
if ($ours) {
    Write-Host ""
    Write-Host "ERROR: module-internal symbols left undefined (a macro lost its"
    Write-Host "       definition but kept a call site). The device would fail to load:"
    $ours | ForEach-Object { Write-Host "         $_" }
    exit 1
}
if ($missing.Count -gt 0 -and $Arch -eq "rv32") {
    Write-Host ""
    Write-Host "ERROR: $($missing.Count) symbol(s) missing from host_exports[] - the elf WILL fail to load:"
    $missing | ForEach-Object { Write-Host "  $_" }
    exit 1
}

# .iram.text audit. Two ways this silently goes wrong, both fatal at runtime:
#   1. a literal left behind in PSRAM -> every l32r in the moved code loads
#      garbage once the loader relocates it (hence -mtext-section-literals)
#   2. a helper de-inlined by its section attribute -> a real call in the
#      innermost loop, a pessimisation dressed as an optimisation
# RISC-V: the loader runs the section in place (PSRAM), so there are no
# literals to audit.
Write-Host ""
$secline = (& $READELF -SW $OUT) -match '\.iram\.text'
if (-not $secline) {
    Write-Host ".iram.text: ABSENT - hot code stayed in PSRAM"
} elseif ($Arch -eq "rv32") {
    $m = [regex]::Match($secline[0], '\.iram\.text\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)')
    $size = [Convert]::ToUInt32($m.Groups[3].Value, 16)
    Write-Host (".iram.text: {0} bytes, run in place from PSRAM on the P4" -f $size)
} else {
    # [ 7] .iram.text  PROGBITS  000336b0 0336b0 000e6c ...  -> addr, off, size
    $m = [regex]::Match($secline[0], '\.iram\.text\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)')
    $addr = [Convert]::ToUInt32($m.Groups[1].Value, 16)
    $size = [Convert]::ToUInt32($m.Groups[3].Value, 16)
    Write-Host (".iram.text: {0} bytes at 0x{1:X} (internal SRAM at load)" -f $size, $addr)

    $dis = & $OBJDUMP -d --section=.iram.text $OUT
    foreach ($line in $dis) {
        if ($line -match '^([0-9a-f]+) <([^>]+)>:') {
            Write-Host ("    {0}" -f $Matches[2])
        }
    }
    # xtensa objdump prints the l32r target bare, with no 0x prefix.
    $bad = 0; $tot = 0
    foreach ($line in $dis) {
        if ($line -match 'l32r\s+a\d+,\s*([0-9a-f]+)') {
            $tot++
            $t = [Convert]::ToUInt32($Matches[1], 16)
            if ($t -lt $addr -or $t -ge ($addr + $size)) { $bad++ }
        }
    }
    if ($tot -eq 0) { Write-Host "  literals: none" }
    elseif ($bad -eq 0) { Write-Host "  literals: $tot/$tot in-section OK" }
    else { Write-Host "  literals: $bad of $tot OUTSIDE section - WILL CRASH (need -mtext-section-literals)" }
}
