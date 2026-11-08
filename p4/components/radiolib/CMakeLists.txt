# components/radiolib — lib/RadioLib 7.8.1, the one copy every board builds
# (D8 of docs/P4_PORT_PLAN_2026-10-12.md, decided 2026-10-14: it carries the
# LR2021 driver this board's LR2021 SKU needs). Its Arduino HAL is selected
# by the ARDUINO define the Arduino component sets; its ESP32 tone
# emulation branches on ESP_IDF_VERSION for the IDF 5 LEDC API. The copy's
# local edits are tagged MESHPUNK: src/BuildOpt.h (RADIOLIB_GODMODE) and
# src/modules/LR2021/LR2021_config.cpp (RADIOLIB_LR2021_CAL_RETRY_DELAY_MS).
set(RADIOLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/RadioLib/src")
file(GLOB_RECURSE RADIOLIB_SOURCES "${RADIOLIB_DIR}/*.cpp")
idf_component_register(
    SRCS ${RADIOLIB_SOURCES}
    INCLUDE_DIRS "${RADIOLIB_DIR}"
    REQUIRES arduino-esp32)
