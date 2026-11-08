# components/crypto — lib/Crypto (rweather/arduinolibs "Crypto" 0.4.0,
# vendored 2026-10-13 from commit 37a76b8f; the S3 boards resolved the same
# version from the PlatformIO registry through lib/MeshCore/library.json).
# MeshCore's Utils.cpp needs its AES and SHA256. On ESP32 targets AES.h
# selects AESEsp32.cpp, which calls the IDF mbedTLS port's esp_aes_setkey
# and esp_aes_crypt_ecb (CONFIG_MBEDTLS_HARDWARE_AES). RNG.cpp keeps its
# seed in NVS.
set(CRYPTO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/Crypto")
file(GLOB CRYPTO_SOURCES "${CRYPTO_DIR}/*.cpp")
idf_component_register(
    SRCS ${CRYPTO_SOURCES}
    INCLUDE_DIRS "${CRYPTO_DIR}"
    REQUIRES arduino-esp32 mbedtls nvs_flash esp_hw_support)
