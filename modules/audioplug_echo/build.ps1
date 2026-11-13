# Build the audio plugin loader's test plugin (echo.c) twice:
#   echo.audioplug.elf  - the test plugin
#   big.audioplug.elf   - the same asking for an internal block no device has
# Template: modules/usbdrv_kbd/build.ps1. No entry point: the loader
# elf_lookup()s the exported `audioplug_ops`. The UND audit must show only
# symbols in audioplug_exports[] (src/audio/audio_plugin.cpp).

$toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
$CC      = "$toolchain\xtensa-esp32s3-elf-gcc.exe"
$READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"

$FLAGS = @(
    "-shared", "-fPIC", "-fno-common", "-Os", "-mlongcalls",
    "-ffunction-sections", "-fdata-sections", "-std=gnu11",
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,-z,nocombreloc"
)

function Build-Plugin($out, $extra) {
    Write-Host "  CC   echo.c -> $out"
    & $CC @FLAGS @extra -o $out "echo.c" "-lgcc"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Compilation failed!"
        exit 1
    }
    Write-Host "Undefined symbols (each must be in audioplug_exports[]):"
    & $READELF --dyn-syms $out | Select-String "\bUND\b" | ForEach-Object {
        $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
        $sym = $parts[$parts.Length - 1]
        if ($sym -and $sym -ne "UND" -and $sym -ne "Name") { Write-Host "  $sym" }
    }
    $ops = & $READELF --dyn-syms $out | Select-String " audioplug_ops$"
    if (-not $ops) {
        Write-Host "ERROR: audioplug_ops is NOT exported by $out"
        exit 1
    }
    Write-Host "audioplug_ops export: OK ($((Get-Item $out).Length) bytes)"
    Write-Host ""
}

Build-Plugin "echo.audioplug.elf" @()
Build-Plugin "big.audioplug.elf" @("-DECHO_BIG_NEEDS")
