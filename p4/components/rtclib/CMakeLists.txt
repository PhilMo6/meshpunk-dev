# components/rtclib — lib/RTClib (Adafruit RTClib 2.1.4, vendored
# 2026-10-13; the S3 boards resolved the same version from the PlatformIO
# registry). The firmware uses its DateTime class (src/main.cpp,
# src/mesh_store.cpp). RTClib.h includes Adafruit_I2CDevice.h from Adafruit
# BusIO.
set(RTCLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/RTClib/src")
file(GLOB RTCLIB_SOURCES "${RTCLIB_DIR}/*.cpp")
idf_component_register(
    SRCS ${RTCLIB_SOURCES}
    INCLUDE_DIRS "${RTCLIB_DIR}"
    REQUIRES arduino-esp32 busio)
