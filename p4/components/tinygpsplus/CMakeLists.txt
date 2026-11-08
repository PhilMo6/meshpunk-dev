# components/tinygpsplus — lib/TinyGPSPlus 1.0.3, the NMEA parser behind
# src/main.cpp's GPS sync.
set(TINYGPS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/TinyGPSPlus/src")
idf_component_register(
    SRCS "${TINYGPS_DIR}/TinyGPS++.cpp"
    INCLUDE_DIRS "${TINYGPS_DIR}"
    REQUIRES arduino-esp32)
