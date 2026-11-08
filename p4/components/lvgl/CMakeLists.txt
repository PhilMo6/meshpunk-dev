# components/lvgl — lib/lvgl (9.1, vendored with MESHPUNK patches) for the
# P4. Configuration comes from lib/lv_conf.h, found the way the S3 build
# finds it: lvgl/src/lv_conf_internal.h includes ../../lv_conf.h, the file
# next to the lvgl folder. The two Arduino filesystem drivers
# (src/libs/fsdrv/lv_fs_arduino_*.cpp) include Arduino's LittleFS and SD
# headers; the other .cpp files under src/ are compiled out by lv_conf.h
# (LV_USE_TFT_ESPI 0, LV_USE_THORVG_INTERNAL 0).
set(LVGL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/lvgl")
file(GLOB_RECURSE LVGL_SOURCES
     "${LVGL_DIR}/src/*.c" "${LVGL_DIR}/src/*.cpp")
# fatfs: lv_fs_arduino_sd.cpp includes Arduino's SD.h, whose sd_diskio.h
# includes diskio.h; the Arduino component requires fatfs privately.
# The firmware's src/ is a private include path: MESHPUNK patch 13
# (docs/LVGL_LOCAL_PATCHES.md) makes the S: driver open files through the
# board's SD backend, so lv_fs_arduino_sd.cpp includes storage/sd_dev.h —
# an include path only, not a component requirement (components/meshpunk
# requires lvgl, so lvgl cannot require it back).
idf_component_register(
    SRCS ${LVGL_SOURCES}
    INCLUDE_DIRS "${LVGL_DIR}"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../src"
    PRIV_REQUIRES arduino-esp32 fatfs)
