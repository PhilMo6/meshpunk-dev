# Build the speech-to-text audio plugin.
#   -Variant s3 (default)  stt.audioplug.elf, the ESP32-S3 boards: the engine
#                          with upstream's S3 assembly kernels
#   -Variant rv32          stt.audioplug.rv32.elf, the T-Display P4: the
#                          portable kernels (STT_KERNELS_C) with their int8
#                          sums on the P4's 128-bit multiply-accumulate
#                          (STT_KERNELS_P4, src/tlib_ops/kernels_p4/, assembled
#                          with the P4's PIE extension: xesppie), the whole
#                          model kept in PSRAM (STT_MODEL_RESIDENT), the
#                          riscv32-esp toolchain with the P4 firmware's ISA
#                          and float ABI (rv32imafc, ilp32f), -mno-relax, and
#                          no -lgcc (the rv32 libgcc.a is not
#                          position-independent: every import must be in
#                          audioplug_exports[])
#   -Variant rv32prof      stt_prof.audioplug.rv32.elf: rv32 with the stage
#                          profile (STT_PROFILE, src/profile.*; a test build:
#                          each file's result ends with an S:profile line)
#   -Variant s3c           stt_c.audioplug.elf: the portable build for the
#                          ESP32-S3, C sums (a test build: the portable code
#                          on the chip the S3 kernels run on)
#   -Variant kcheck        kcheck.audioplug.elf: test/kcheck.cpp with the S3
#                          kernels and the portable ones side by side
#   -Variant kcheck_p4     kcheck_p4.audioplug.rv32.elf: test/kcheck_p4.cpp,
#                          the P4's int8 sums (and test/kcheck_p4_loops.S and
#                          test/kcheck_p4_hwloop.S, the same with the block
#                          loop rearranged and on the zero-overhead loop)
#                          against C sums, and tlib_kernels_c.cpp's float
#                          kernel against its earlier form
# s3 and rv32 are copied into the Voice to Text app's folder in data/: the
# app loads stt.audioplug.elf, and the P4's loader opens the .rv32.elf beside
# it (elf_arch_path); data_filter.py gives each board's image its own.
#   src/            the plugin (plugin.cpp), its config and the engine seam (port.h)
#   src/tlib*       upstream conformer-stt-s3's tensor library, ops and kernels
#                   (tlib_kernels_c.* the portable kernels)
#   src/conformer   upstream's model code
#   src/dsp         esp-dsp's FFT: the code its ESP32-S3 build runs, and the
#                   same in C (fft4r_c.cpp, exact_angle.*)
# Template: modules/audioplug_echo/build.ps1. This one is C++ (gnu++17, no
# RTTI, exceptions or thread-safe statics), -O2 as upstream's sdkconfig
# (CONFIG_COMPILER_OPTIMIZATION_PERF), hidden visibility with audioplug_ops
# the one export, and -Bsymbolic so calls into the kernels bind inside the
# module. The UND audit must show only names in audioplug_exports[]
# (src/audio/audio_plugin.cpp). The stack summary lists each function's frame
# (-fstack-usage); the worker's stack is 8 KB.
param(
    [ValidateSet("s3", "rv32", "rv32prof", "s3c", "kcheck", "kcheck_p4")]
    [string]$Variant = "s3"
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$RV32 = ($Variant -eq "rv32" -or $Variant -eq "rv32prof" -or $Variant -eq "kcheck_p4")
if ($RV32) {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-riscv32-esp\bin"
    $CC      = "$toolchain\riscv32-esp-elf-gcc.exe"
    $CXX     = "$toolchain\riscv32-esp-elf-g++.exe"
    $READELF = "$toolchain\riscv32-esp-elf-readelf.exe"
    $ARCH    = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-mno-relax")
    $ARCH_LD = @("-Wl,--no-relax")
    $LIBS    = @()
    # The P4 kernels' assembler: the same ISA plus the PIE extension.
    $ARCH_PIE = @("-march=rv32imafc_zicsr_zifencei_xesppie", "-mabi=ilp32f", "-mno-relax")
} else {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
    $CC      = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
    $CXX     = "$toolchain\xtensa-esp32s3-elf-g++.exe"
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    $ARCH    = @("-mlongcalls")
    $ARCH_LD = @()
    $LIBS    = @("-lgcc")
}

$OUT = switch ($Variant) {
    "s3"        { "stt.audioplug.elf" }
    "rv32"      { "stt.audioplug.rv32.elf" }
    "rv32prof"  { "stt_prof.audioplug.rv32.elf" }
    "s3c"       { "stt_c.audioplug.elf" }
    "kcheck"    { "kcheck.audioplug.elf" }
    "kcheck_p4" { "kcheck_p4.audioplug.rv32.elf" }
}
$OBJ = Join-Path "build" $Variant

$DEFS = @()
if ($Variant -eq "s3c") { $DEFS = @("-DSTT_KERNELS_C") }
if ($Variant -eq "rv32") { $DEFS = @("-DSTT_KERNELS_C", "-DSTT_KERNELS_P4", "-DSTT_MODEL_RESIDENT") }
if ($Variant -eq "rv32prof") { $DEFS = @("-DSTT_KERNELS_C", "-DSTT_KERNELS_P4", "-DSTT_MODEL_RESIDENT", "-DSTT_PROFILE") }

$INC = @("-Isrc", "-Isrc/tlib/inc", "-Isrc/tlib_ops/inc", "-Isrc/conformer/inc")
$COMMON = $ARCH + $DEFS + @(
    "-fPIC", "-fno-common", "-O2",
    "-ffunction-sections", "-fdata-sections",
    "-fvisibility=hidden", "-fstack-usage", "-Wall"
)
$CXXFLAGS = $COMMON + @(
    "-std=gnu++17", "-fno-rtti", "-fno-exceptions",
    "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
    "-fvisibility-inlines-hidden"
) + $INC
$CFLAGS = $COMMON + @("-std=gnu11") + $INC
$LDFLAGS = @(
    "-shared", "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,-z,nocombreloc",
    "-Wl,-Bsymbolic"
) + $ARCH_LD

# Sources by role (paths under src/).
$S3_ONLY   = @("tlib_ops\src\tlib_linear_impl_esp32s3.cpp", "tlib_ops\src\tlib_quantize_impl_esp32s3.cpp",
               "dsp\fft4r_aes3.S", "dsp\cplx2real_ae32.c")
$C_IMPL    = @("tlib_ops\src\tlib_linear_impl_c.cpp", "tlib_ops\src\tlib_quantize_impl_c.cpp")
$C_KERNELS = @("tlib_ops\src\tlib_kernels_c.cpp", "dsp\fft4r_c.cpp", "dsp\exact_angle.cpp")
$KCHECK    = $C_KERNELS + @("dsp\fft4r_aes3.S", "dsp\cplx2real_ae32.c", "dsp\fft4r.cpp")
$PROF_ONLY = @("profile.cpp")

$srcRoot = (Resolve-Path "src").Path
$all = Get-ChildItem -Path src -Recurse -File | Where-Object { $_.Extension -in @(".cpp", ".c", ".s", ".S") }
$sources = @()
foreach ($f in $all) {
    $rel = $f.FullName.Substring($srcRoot.Length + 1)
    $s3Kernel = $rel.StartsWith("tlib_ops\kernels\")
    $p4Kernel = $rel.StartsWith("tlib_ops\kernels_p4\")
    $prof = $PROF_ONLY -contains $rel
    $take = switch ($Variant) {
        "s3"        { -not $p4Kernel -and -not $prof -and -not ($C_IMPL -contains $rel) -and -not ($C_KERNELS -contains $rel) }
        "rv32"      { -not $s3Kernel -and -not $prof -and -not ($S3_ONLY -contains $rel) }
        "rv32prof"  { -not $s3Kernel -and -not ($S3_ONLY -contains $rel) }
        "s3c"       { -not $s3Kernel -and -not $p4Kernel -and -not $prof -and -not ($S3_ONLY -contains $rel) }
        "kcheck"    { $s3Kernel -or ($KCHECK -contains $rel) }
        "kcheck_p4" { $p4Kernel -or $rel -eq "tlib_ops\src\tlib_kernels_c.cpp" }
    }
    if ($take) { $sources += $f }
}
if ($Variant -eq "kcheck") { $sources += Get-Item "test\kcheck.cpp" }
if ($Variant -eq "kcheck_p4") {
    $sources += Get-Item "test\kcheck_p4.cpp"
    $sources += Get-Item "test\kcheck_p4_loops.S"
    $sources += Get-Item "test\kcheck_p4_hwloop.S"
}

if (Test-Path $OBJ) { Remove-Item -Recurse -Force $OBJ }
New-Item -ItemType Directory -Force $OBJ | Out-Null

$objs = @()
foreach ($f in $sources) {
    $rel = if ($f.FullName.StartsWith($srcRoot)) { $f.FullName.Substring($srcRoot.Length + 1) } else { "test\" + $f.Name }
    $o = Join-Path $OBJ (($rel -replace '[\\/]', '_') + ".o")
    switch -CaseSensitive ($f.Extension) {
        ".cpp" { $cmd = $CXX; $cargs = $CXXFLAGS + @("-c", $f.FullName, "-o", $o) }
        ".c"   { $cmd = $CC;  $cargs = $CFLAGS + @("-c", $f.FullName, "-o", $o) }
        ".S"   {
            $cmd = $CC
            $asArch = if ($RV32) { $ARCH_PIE } else { @() }
            $cargs = $asArch + $INC + @("-x", "assembler-with-cpp", "-c", $f.FullName, "-o", $o)
        }
        ".s"   { $cmd = $CC;  $cargs = @("-x", "assembler", "-c", $f.FullName, "-o", $o) }
    }
    Write-Host "  $($f.Extension.TrimStart('.').ToUpper().PadRight(4)) $rel"
    & $cmd @cargs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Compilation failed: $rel"
        exit 1
    }
    $objs += $o
}

Write-Host "  LD   $OUT"
& $CXX @LDFLAGS -o $OUT @objs @LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed!"
    exit 1
}
Write-Host "Success: $OUT ($((Get-Item $OUT).Length) bytes)"

# The host's export names, read from its table.
$hostSrc = Get-Content "..\..\src\audio\audio_plugin.cpp" -Raw
$block = [regex]::Match($hostSrc, 'audioplug_exports\[\]\s*=\s*\{(.*?)\n\};', 'Singleline').Groups[1].Value
$exports = [regex]::Matches($block, '\{\s*"([A-Za-z_0-9]+)"') | ForEach-Object { $_.Groups[1].Value }

Write-Host ""
Write-Host "Undefined symbols (each must be in audioplug_exports[]):"
$missing = 0
& $READELF --dyn-syms -W $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") {
        if ($exports -contains $sym) {
            Write-Host "  $sym"
        } else {
            Write-Host "  $sym   <-- NOT EXPORTED BY THE HOST"
            $missing++
        }
    }
}
if ($missing -gt 0) {
    Write-Host "ERROR: $missing import(s) the host does not export"
    exit 1
}

$ops = & $READELF --dyn-syms -W $OUT | Select-String " audioplug_ops$"
if (-not $ops) {
    Write-Host "ERROR: audioplug_ops is NOT exported by $OUT"
    exit 1
}
Write-Host "audioplug_ops export: OK"

if ($Variant -eq "s3" -or $Variant -eq "rv32") {
    $APPDIR = "..\..\data\lua\apps\Tools\Voice to Text"
    Copy-Item $OUT (Join-Path $APPDIR $OUT) -Force
    Write-Host "Copied to $APPDIR\$OUT"
}

Write-Host ""
Write-Host "Sections:"
& $READELF -S -W $OUT | Select-String "\.text|\.iram|\.rodata|\.data|\.bss|init_array|ctors|\.got|\.rela" | ForEach-Object { Write-Host "  $($_.Line.Trim())" }

Write-Host ""
Write-Host "Largest stack frames (bytes):"
Get-ChildItem $OBJ -Filter *.su | ForEach-Object { Get-Content $_.FullName } | ForEach-Object {
    $cols = $_ -split "`t"
    if ($cols.Length -ge 3) {
        [pscustomobject]@{ Fn = ($cols[0] -split ':')[-1]; Bytes = [int]$cols[1]; Kind = $cols[2] }
    }
} | Sort-Object Bytes -Descending | Select-Object -First 25 | ForEach-Object {
    Write-Host ("  {0,6}  {1,-8} {2}" -f $_.Bytes, $_.Kind, $_.Fn)
}
