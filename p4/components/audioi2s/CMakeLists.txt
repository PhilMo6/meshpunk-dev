# components/audioi2s — lib/ESP32-audioI2S 2.0.6 (vendored with MESHPUNK
# patches): the MP3/AAC/FLAC decoder and I2S player src/sound.cpp is built
# on, which includes Audio.h on every board. It drives IDF's legacy I2S
# driver (driver/i2s.h, the `driver` component; sdkconfig.defaults sets
# CONFIG_I2S_SUPPRESS_DEPRECATE_WARN). In Phase 0 no P4 code creates a
# player (audio_tdisplay_p4.cpp); Phase 4 decides the P4 audio path.
set(AUDIOI2S_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/ESP32-audioI2S/src")
file(GLOB_RECURSE AUDIOI2S_SOURCES "${AUDIOI2S_DIR}/*.cpp")
# Audio.h includes Arduino's FFat.h, SD.h and SD_MMC.h, which include
# wear_levelling.h, diskio.h (fatfs) and driver/sdmmc_types.h; the Arduino
# component requires those IDF components privately, so they are named
# here.
idf_component_register(
    SRCS ${AUDIOI2S_SOURCES}
    INCLUDE_DIRS "${AUDIOI2S_DIR}"
    REQUIRES arduino-esp32 driver fatfs wear_levelling sdmmc esp_driver_sdmmc)
