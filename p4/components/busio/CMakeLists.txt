# components/busio — lib/Adafruit_BusIO (Adafruit BusIO 1.17.4, vendored
# 2026-10-13; the S3 boards resolved the same version from the PlatformIO
# registry), RTClib's I2C/SPI device layer. The library's own CMakeLists.txt
# targets a project with Arduino registered as a component named "arduino"
# and is not used here.
set(BUSIO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/Adafruit_BusIO")
file(GLOB BUSIO_SOURCES "${BUSIO_DIR}/*.cpp")
idf_component_register(
    SRCS ${BUSIO_SOURCES}
    INCLUDE_DIRS "${BUSIO_DIR}"
    REQUIRES arduino-esp32)
