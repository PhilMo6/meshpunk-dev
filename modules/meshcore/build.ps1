# Build the meshcore LoRa-protocol package: lib/MeshCore (pristine, compiled
# against the shim/ Arduino environment) + the rweather Crypto sources it
# uses (MIT; same software crypto the firmware links) + the package TUs, as
# meshcore.loraproto.elf, plus the companion BLE protocol as its own
# meshcore_companion.bleproto.elf.
#
# Same link rules as the mtlite module: no entry point (loader elf_lookup()s
# loraproto_ops), --no-relax + -z nocombreloc, -lm -lgcc static. The UND
# audits below cross-check against proto_exports[] (src/elf_host.cpp) and
# hard-fail on any symbol the firmware does not export.
#
# -Arch s3 (default) builds the Xtensa artifacts for the S3 boards;
# -Arch rv32 builds the RISC-V artifacts for the T-Display P4
# (docs/DEVICE_EXPANSION_PLAN_2026-09-08.md 3.1 / 3.4): same sources, the
# riscv32-esp toolchain, the P4 firmware's ISA and float ABI
# (rv32imafc, ilp32f; -mno-relax so no linker-relaxation relocations),
# output names tagged ".rv32.elf" (the firmware's ELF_ARCH_EXT, elf_host.h)
# staged next to the Xtensa files. No -lm/-lgcc on rv32: the toolchain's
# libgcc.a/libm.a are not position-independent and ld refuses them in a
# shared object, so every builtin a module needs comes from proto_exports[].
param(
    [ValidateSet("s3", "rv32")]
    [string]$Arch = "s3"
)

if ($Arch -eq "rv32") {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-riscv32-esp\bin"
    $CXX     = "$toolchain\riscv32-esp-elf-g++.exe"
    $READELF = "$toolchain\riscv32-esp-elf-readelf.exe"
    $ARCH_EXT = ".rv32.elf"
    $ARCH_CFLAGS = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-mno-relax")
    $LIBS = @()
} else {
    $toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin"
    $CXX     = "$toolchain\xtensa-esp32s3-elf-g++.exe"
    $READELF = "$toolchain\xtensa-esp32s3-elf-readelf.exe"
    $ARCH_EXT = ".elf"
    $ARCH_CFLAGS = @("-mlongcalls")
    $LIBS = @("-lm", "-lgcc")
}

$OUT  = "meshcore.loraproto$ARCH_EXT"
$OUT2 = "meshcore_companion.bleproto$ARCH_EXT"

$MC     = "..\..\lib\MeshCore\src"
# Vendored in lib/ (the same versions the S3 envs' libdeps hold: Crypto
# 0.4.0, base64 1.4.0), so the module builds on a machine without libdeps.
$CRYPTO = "..\..\lib\Crypto"
$BASE64 = "..\..\lib\base64\src"

# The BLE-slot component is its OWN elf (one loading mechanism for every BLE
# protocol); it resolves its PunkMesh/shim imports against the LoRa elf at
# load time.
$SRC2 = @(
    "mc_bleproto.cpp",
    "ble_companion.cpp",
    "ble_msg_sync.cpp"
)

$SRC = @(
    "mcmain.cpp",
    "mcshim.cpp",
    "mclua.cpp",
    "punkmesh.cpp",
    "$MC\Dispatcher.cpp",
    "$MC\Mesh.cpp",
    "$MC\Packet.cpp",
    "$MC\Identity.cpp",
    "$MC\Utils.cpp",
    "$MC\helpers\BaseChatMesh.cpp",
    "$MC\helpers\StaticPoolPacketManager.cpp",
    "$MC\helpers\IdentityStore.cpp",
    "$MC\helpers\TransportKeyStore.cpp",
    "$MC\helpers\AdvertDataHelpers.cpp",
    "$MC\helpers\TxtDataHelpers.cpp",
    "$CRYPTO\SHA256.cpp",
    "$CRYPTO\SHA512.cpp",
    "$CRYPTO\AES128.cpp",
    "$CRYPTO\AESCommon.cpp",
    "$CRYPTO\BlockCipher.cpp",
    "$CRYPTO\Cipher.cpp",
    "$CRYPTO\Crypto.cpp",
    "$CRYPTO\Hash.cpp",
    "$CRYPTO\Ed25519.cpp",
    "$CRYPTO\Curve25519.cpp",
    "$CRYPTO\BigNumberUtil.cpp"
)

$FLAGS = $ARCH_CFLAGS + @(
    "-shared", "-fPIC", "-fno-common", "-Os",
    "-ffunction-sections", "-fdata-sections",
    "-fno-rtti", "-fno-exceptions", "-std=gnu++17",
    # The companion is part of the package, over the BLE-slot transport.
    "-DBLE_COMPANION_ENABLED=1",
    # Firmware build flags mirrored (platformio.ini). MAX_CONTACTS MUST
    # match firmware: a smaller table mass-archives the surplus contacts
    # during loadContacts() on every boot (the 5f watchdog loop).
    "-DMAX_CONTACTS=500",
    "-DMAX_GROUP_CHANNELS=20",
    "-DBLE_PIN_CODE=123456",
    "-DLORA_FREQ=910.525", "-DLORA_BW=62.5", "-DLORA_SF=7",
    "-DLORA_TX_POWER=20",
    # Baked default only: the firmware forwards the real per-board cap at
    # boot via set_config("max_tx_dbm") (20 tdeck / 22 heltec).
    "-DMAX_LORA_TX_POWER=22",
    # Selects IdentityStore's fs::FS branch WITHOUT -DESP32 (which would
    # flip Crypto's AES.h to the hardware class this build cannot link).
    "-DRP2040_PLATFORM=1",
    "-I.",
    "-Ishim",                # shim env SHADOWS same-named src/ headers
    "-I..\..\src\radio",
    "-I..\..\src",           # mesh_store.h (types + mangled-export decls)
    "-I..\..\lib\lua",
    "-I$MC",
    "-I$CRYPTO",
    "-I$BASE64",
    "-I..\..\lib\ed25519",
    "-nostartfiles", "-nodefaultlibs", "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--no-relax",
    "-Wl,-z,nocombreloc"
)

Write-Host "  CXX  $($SRC.Count) sources -> $OUT"
& $CXX @FLAGS -o $OUT @SRC @LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation failed!"
    exit 1
}

$size = (Get-Item $OUT).Length
Write-Host "Success: $OUT ($([math]::Round($size / 1KB)) KB)"

Write-Host ""
# UND audit, cross-checked against the ACTUAL proto_exports[] table (the
# strncasecmp lesson: eyeballing once passed a symbol that only existed in
# the game-module table).
$hostSrc = Get-Content (Join-Path $PSScriptRoot "..\..\src\elf_host.cpp") -Raw
$tbl = [regex]::Match($hostSrc, 'proto_exports\[\]\s*=\s*\{(.*?)\n\};', 'Singleline')
if (-not $tbl.Success) {
    Write-Host "ERROR: could not parse proto_exports[] from src/elf_host.cpp"
    exit 1
}
$exported = @{}
foreach ($em in [regex]::Matches($tbl.Groups[1].Value, '\{\s*"([^"]+)"')) {
    $exported[$em.Groups[1].Value] = $true
}
# Entries whose linkage name differs per architecture: U32_MANGLED(xtensa, riscv)
# in src/elf_host.cpp (uint32_t mangles as 'j' on Xtensa, 'm' on RISC-V).
$grp = if ($Arch -eq "rv32") { 2 } else { 1 }
foreach ($em in [regex]::Matches($tbl.Groups[1].Value, 'U32_MANGLED\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)')) {
    $exported[$em.Groups[$grp].Value] = $true
}

$missing = @()
Write-Host "Undefined symbols (checked against proto_exports[], src/elf_host.cpp):"
# -W: default readelf output truncates long names and false-fails the lookup.
& $READELF -W --dyn-syms $OUT | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") {
        if ($exported.ContainsKey($sym)) {
            Write-Host "  $sym"
        } else {
            Write-Host "  $sym   <-- NOT IN proto_exports[]"
            $missing += $sym
        }
    }
}
if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "ERROR: $($missing.Count) symbol(s) missing from proto_exports[] - the elf WILL fail to load:"
    $missing | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "UND audit: all resolved by proto_exports[]"

Write-Host ""
$ops = & $READELF -W --dyn-syms $OUT | Select-String " loraproto_ops$"
if ($ops) {
    Write-Host "loraproto_ops export: OK"
} else {
    Write-Host "ERROR: loraproto_ops is NOT exported!"
    exit 1
}

# ── The companion elf (BLE slot) ────────────────────────────────────────────
Write-Host ""
Write-Host "  CXX  $($SRC2.Count) sources -> $OUT2"
& $CXX @FLAGS -o $OUT2 @SRC2 @LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Host "Companion compilation failed!"
    exit 1
}
$size2 = (Get-Item $OUT2).Length
Write-Host "Success: $OUT2 ($([math]::Round($size2 / 1KB)) KB)"

# UND audit #2: resolves from proto_exports[] OR the LoRa elf's own dynsym
# (the load-time cross-module fallback) — anything else fails the build.
$loraSyms = @{}
& $READELF -W --dyn-syms $OUT | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    if ($parts.Length -ge 8 -and $parts[6] -ne "UND") {
        $loraSyms[$parts[7]] = $true
    }
}
$missing2 = @()
Write-Host ""
Write-Host "Companion UND (vs proto_exports[] + $OUT dynsym):"
& $READELF -W --dyn-syms $OUT2 | Select-String "\bUND\b" | ForEach-Object {
    $parts = ($_ -replace '\s+', ' ').Trim().Split(' ')
    $sym = $parts[$parts.Length - 1]
    if ($sym -and $sym -ne "UND" -and $sym -ne "Name") {
        if ($exported.ContainsKey($sym)) {
            Write-Host "  $sym"
        } elseif ($loraSyms.ContainsKey($sym)) {
            Write-Host "  $sym   (from LoRa elf)"
        } else {
            Write-Host "  $sym   <-- UNRESOLVABLE"
            $missing2 += $sym
        }
    }
}
if ($missing2.Count -gt 0) {
    Write-Host ""
    Write-Host "ERROR: $($missing2.Count) companion symbol(s) unresolvable:"
    $missing2 | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "Companion UND audit: all resolvable"

$ops2 = & $READELF -W --dyn-syms $OUT2 | Select-String " bleproto_ops$"
if ($ops2) {
    Write-Host "bleproto_ops export: OK"
} else {
    Write-Host "ERROR: bleproto_ops is NOT exported!"
    exit 1
}

# ── Stage into the firmware data tree ───────────────────────────────────────
# meshcore ships preinstalled via the data/ image (the .version marker there
# is the store's update tracking — Copy-Item never touches it). Removing the
# staged elf via Tools > Files boots the no-radio floor with a notice.
# A catalog release ALSO needs the elfs copied to
# meshpunk-apps-dev/protocols/meshcore/ and the sources to
# meshpunk-apps-dev/module-src/meshcore/ (version bumped in its catalog.toml;
# the stable catalog gets it through that repo's promote script).
$D1 = "..\..\data\meshpunk\lora_protos\meshcore"
$D2 = "..\..\data\meshpunk\ble_protos\meshcore_companion"
New-Item -ItemType Directory -Force $D1 | Out-Null
New-Item -ItemType Directory -Force $D2 | Out-Null
Copy-Item $OUT  (Join-Path $D1 $OUT)  -Force
Copy-Item $OUT2 (Join-Path $D2 $OUT2) -Force
$c1 = (Get-FileHash $OUT).Hash -eq (Get-FileHash (Join-Path $D1 $OUT)).Hash
$c2 = (Get-FileHash $OUT2).Hash -eq (Get-FileHash (Join-Path $D2 $OUT2)).Hash
if ($c1 -and $c2) {
    Write-Host ""
    Write-Host "Staged: data/meshpunk/lora_protos/meshcore/ + data/meshpunk/ble_protos/meshcore_companion/"
} else {
    Write-Host "ERROR: staged copies do not match!"
    exit 1
}
