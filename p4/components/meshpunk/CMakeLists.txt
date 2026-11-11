# components/meshpunk — the firmware (../../../src) as one IDF component.
#
# The S3 boards build src/ through PlatformIO (platformio.ini); this is the
# same tree for the T-Display P4, with the board define BOARD_TDISPLAY_P4
# set build-wide in p4/CMakeLists.txt. Sources are globbed at configure
# time, so a new file in src/ needs no edit here but does need a
# reconfigure: IDF reads this file in CMake script mode first
# (tools/cmake/scripts/component_get_requirements.cmake), where file(GLOB)
# may not carry CONFIGURE_DEPENDS. pioarduino reconfigures only when
# p4/CMakeLists.txt, p4/main/CMakeLists.txt, sdkconfig.defaults or
# dependencies.lock is newer than its CMake cache (builder/frameworks/
# espidf.py, is_cmake_reconfigure_required), so after adding a source file
# touch one of those, or the build links against the old file list.
#
# Object names: the same builder names each object after the source's path
# relative to the IDF tree, the build directory or p4/, and by bare file
# name for everything else (compile_source_files) — which is every file
# under src/ and lib/. Two same-named sources across those trees stop the
# build with "Multiple ways to build the same target"; the one pair today
# (MeshCore's and RadioLib's Utils.cpp) is handled in components/meshcore.
set(MESHPUNK_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../src")
file(GLOB_RECURSE MESHPUNK_SOURCES
     "${MESHPUNK_SRC}/*.c" "${MESHPUNK_SRC}/*.cpp" "${MESHPUNK_SRC}/*.S")

# Not in the P4 build:
#  - src/updater/: the standalone updater firmware (its own envs on the S3
#    boards); it defines its own setup()/loop().
# The USB-OTG subsystem (src/usb/, usb_fs.cpp, usb_manager.cpp) and the peer
# link (tdeck_link.cpp) build for this board: the P4's USB 2.0 OTG
# high-speed pair reaches the USB-C port USB1 and the USB-A host socket (the
# board schematic; docs/P4_PORT_PLAN_2026-10-12.md §9). Every other
# board-specific file compiles to nothing outside its own BOARD_<NAME>
# define and needs no exclusion.
list(FILTER MESHPUNK_SOURCES EXCLUDE REGEX "/src/updater/")

idf_component_register(
    SRCS ${MESHPUNK_SOURCES}
    INCLUDE_DIRS "${MESHPUNK_SRC}"
    REQUIRES
        # the vendored libraries (p4/components/<name> -> ../../../lib/<lib>)
        arduino-esp32 lvgl luavgl lua meshcore ed25519 elf_loader radiolib
        tinygpsplus audioi2s crypto rtclib busio
        # ESP-IDF components src/ includes directly
        esp_mm esp_partition app_update mbedtls esp_wifi lwip esp_netif
        driver esp_driver_gpio esp_hw_support esp_system esp_timer heap
        freertos soc esp_rom spi_flash nvs_flash vfs esp_psram
        # the T-Display P4 board layer and display backend: MIPI-DSI panel
        # (esp_lcd_mipi_dsi.h), the I2C master driver (driver/i2c_master.h;
        # `driver` requires it publicly, named here for the include anyway),
        # esp-hosted's transport config (the C6 reset callback,
        # esp_hosted_transport_config.h — libraries/esp_hosted)
        esp_lcd esp_driver_i2c esp_hosted
        # ESP-IDF components the Arduino storage headers include (Audio.h
        # brings in FFat.h -> wear_levelling.h, SD.h -> diskio.h (fatfs),
        # SD_MMC.h -> driver/sdmmc_types.h); the Arduino component requires
        # them privately, so they do not propagate to us.
        fatfs wear_levelling sdmmc esp_driver_sdmmc
        # the USB host library (src/usb/usb_core.cpp) and esp_tinyusb with
        # the TinyUSB core (src/usb/usb_msc_dev.cpp, drive mode)
        usb espressif__esp_tinyusb espressif__tinyusb)
