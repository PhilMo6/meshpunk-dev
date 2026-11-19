# updater — the standalone updater program (../../src/updater/main_updater.cpp)
# for the `updater` partition, as one IDF component: the program, the board
# layer, the panel backend, the SDMMC backend and SLog. Arduino's
# cores/esp32/main.cpp is app_main() and runs its setup()/loop().
#
# The project adds this directory only for the updater image
# (../CMakeLists.txt, -DMESHPUNK_UPDATER=1); the firmware build never sees it.
# The requirements below are unconditional: IDF reads them in an early
# script-mode pass that does not see the project's cache variables.
set(MESHPUNK_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(MESHPUNK_LIB "${CMAKE_CURRENT_LIST_DIR}/../../lib")

idf_component_register(
    SRCS "${MESHPUNK_SRC}/updater/main_updater.cpp"
         "${MESHPUNK_SRC}/meshpunk_sync.cpp"
         "${MESHPUNK_SRC}/boards/tdisplay_p4_board.cpp"
         "${MESHPUNK_SRC}/display/display_tdisplay_p4.cpp"
         "${MESHPUNK_SRC}/storage/sd_tdisplay_p4.cpp"
    INCLUDE_DIRS
         "${MESHPUNK_SRC}"
         # <Fonts/Font16.h>: the glyph table of the updater's panel text
         "${MESHPUNK_LIB}/TFT_eSPI"
         # <MeshCore.h>, which meshpunk_sync.h includes (headers only)
         "${MESHPUNK_LIB}/MeshCore/src"
    REQUIRES
         arduino-esp32 esp_lcd esp_driver_i2c esp_hosted esp_hw_support
         esp_mm esp_psram fatfs sdmmc esp_driver_sdmmc app_update mbedtls
         esp_partition driver esp_driver_gpio esp_system esp_rom freertos
         vfs)
