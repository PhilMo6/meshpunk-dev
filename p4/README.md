# Meshpunk P4 — the ESP32-P4 build tree

The T-Display P4 port's ESP-IDF project (plan: docs/DEVICE_EXPANSION_PLAN_2026-09-08.md §6).
This tree is separate from platformio.ini by design — the P4 needs ESP-IDF
5.5.4+ with Arduino as a component, which PlatformIO's espressif32@6.11
cannot provide. The S3 boards keep building exactly as before.

Current state: **bring-up rung 1** — boot banner, chip/flash/PSRAM report,
an 8MB PSRAM write/readback sweep with bandwidth, 1s heartbeat. No display,
no radio, no Arduino, no meshpunk src/ yet; those arrive rung by rung
(§6.15 ladder) once this flashes and reports clean.

## Build in VS Code (PlatformIO) — the day-to-day path

This folder is its own PlatformIO project (`platformio.ini` here wraps the
IDF tree; the platform pin is pioarduino 55.03.312 = ESP-IDF v5.5.5).

1. VS Code → File → **Add Folder to Workspace** → `meshpunk-dev/p4`. The
   PlatformIO toolbar's project/env picker then lists `meshpunk_p4` next to
   the S3 envs.
2. Build / Upload / Monitor buttons work as on the other envs. The first
   build downloads the IDF 5.5.5 framework + P4 toolchain (large, one-time).
3. Two USB-C ports: **"P4.U" (right side) is the data port** — flashing,
   serial monitor, the COM port that enumerates. "U" (left side) is
   charging/power only, no data. If flashing fails to sync: hold BOOT, tap
   RST, release BOOT, flash again. If a serial terminal hangs the board,
   disable RTS/hardware flow control (LilyGo-documented quirk; PlatformIO's
   monitor is normally fine).
4. `pio run -t menuconfig` opens menuconfig when a Kconfig change is needed;
   durable settings belong in sdkconfig.defaults.
5. **Capturing boot text**: do NOT unplug/replug — on this port the monitor's
   handle dies across re-enumeration and never receives the new device
   (the T-Deck replug ritual does not work here). Instead reset the chip
   from inside the attached monitor: `Ctrl+T Ctrl+D` (DTR off),
   `Ctrl+T Ctrl+R` (reset held), `Ctrl+T Ctrl+R` (released — boots),
   `Ctrl+T Ctrl+D` (DTR back on). The full boot streams into the same
   session.
6. Config-flip discipline: after changing sdkconfig.defaults, delete the
   generated `sdkconfig.meshpunk_p4` AND `.pio\build\meshpunk_p4`, then
   rebuild — stale artifacts around config flips have produced misleading
   failures.

## Build with bare idf.py (alternative)

The same tree builds with a standalone ESP-IDF v5.5.4+ install
(install.bat esp32p4, then the IDF shell):

```
cd meshpunk-dev\p4
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Exit the monitor with Ctrl+]. The two entry points share
sdkconfig.defaults and the partition CSV; they keep separate build dirs
(`.pio/` vs `build/`), which is fine. UNTESTED: no bare-IDF install has
run against this tree yet — the root CMakeLists is the canonical 3-line
IDF form, but this path is a claim, not a verified fact, until someone
runs it.

## Expected rung-1 output

```
=== Meshpunk P4 — bring-up rung 1 ===
[chip] ESP32-P4, 2 cores, rev vX.Y
[flash] 16 MB
[psram] 32 MB mapped
[heap] internal: free ..., largest ...
[heap] psram:    free ..., largest ...
[psram] sweep 8MB: PASS, write ...ms (... MB/s), read ...ms (... MB/s)
[hb] up 0s
```

A FAIL from the sweep or a psram size of 0 means the SPIRAM hex/200MHz
config and the module disagree — stop and diagnose before any further rung.

## Envs, flash layout, release files

`platformio.ini` has three envs:

| env | builds | partition |
|---|---|---|
| `meshpunk_p4` | the firmware without the data pack (dev build) | `main`, 0x10000 |
| `meshpunk_p4_updater` | the updater program (`../src/updater/main_updater.cpp`, IDF component `updater/`, settings `sdkconfig.defaults` + `sdkconfig.updater`) | `updater`, 0x590000 |
| `meshpunk_p4_release` | the firmware with the first-boot data pack embedded, and the release files | `main`, 0x10000 |

Flash layout (`partitions_meshpunk_p4.csv`; the partitions sit where the S3
boards' `meshpunk_custom_16Mb.csv` puts them):

| offset | size | what |
|---|---|---|
| 0x2000 | | second-stage bootloader (0x0 on the S3 boards) |
| 0x8000 | | partition table |
| 0x9000 | 0x5000 | `nvs` |
| 0xE000 | 0x2000 | `otadata`: which app the bootloader starts; blank = `updater` |
| 0x10000 | 0x580000 | `main` (ota_0): the firmware |
| 0x590000 | 0x80000 | `updater` (factory): the updater program |
| 0x610000 | 0x9F0000 | `assets`: the LittleFS drive |

On-device updates work as on the S3 boards (`../src/ota_update.cpp`,
`../src/updater/main_updater.cpp`): the firmware stages and verifies an
image, blanks otadata and restarts; the updater writes the image into
`main` and starts it. With no update job the updater installs a release
`-firmware.bin` for this board found in `S:/meshpunk/ota/`, else it starts
the firmware.

Release files, written to `../releases/` by `../merge_bin.py`:

```
pio run -e meshpunk_p4_updater
pio run -e meshpunk_p4 -t buildfs
pio run -e meshpunk_p4_release
```

| file | write at |
|---|---|
| `meshpunk-tdisplay_p4-<version>-merged.bin` | 0x0: the whole flash (bootloader, table, an otadata that selects `main`, firmware, updater, drive) |
| `meshpunk-tdisplay_p4-<version>-firmware.bin` | 0x10000; also the file the on-device updater installs |
| `meshpunk-tdisplay_p4-<version>-updater.bin` | 0x590000 |
| `meshpunk-tdisplay_p4-<version>-littlefs.bin` | 0x610000 |

The release env runs `merge_bin.py` when it builds `firmware.bin`; when
nothing needed rebuilding, `pio run -e meshpunk_p4_release -t mergebin`
runs it alone.

Uploads:

- `pio run -e meshpunk_p4_updater -t upload --upload-port COMx` writes the
  updater at 0x590000 (the env's own esptool command).
- `pio run -e meshpunk_p4 -t upload` writes bootloader, table and firmware,
  and with them a blank otadata (`ota_data_initial.bin`, this platform's
  espidf builder). The next start therefore goes through the updater
  first. An esptool write of `firmware.bin` at 0x10000 alone leaves
  otadata as it is.

## Notes

- sdkconfig.defaults carries the board vendor's memory configuration
  (hex-mode PSRAM @200MHz, 256KB L2 cache) from Xinyuan-LilyGO/T-Display-P4.
- Factory recovery: LilyGo's prebuilt images (their repo `firmware/`
  directory) reflash the stock demo at offset 0x0 via Espressif's web
  flasher.
