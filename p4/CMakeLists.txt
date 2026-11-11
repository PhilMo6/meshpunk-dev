cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshpunk_p4)

# Build-wide definitions: the IDF form of platformio.ini's build_flags
# ([meshpunk_base] plus a board block). They reach every component, Arduino
# and IDF included, as PlatformIO's build_flags reach the framework too;
# component registration reads this property through a generator expression
# (tools/cmake/component.cmake), so appending after project() is the
# documented place. The S3 flags only the mesh protocol reads (MAX_CONTACTS,
# OFFLINE_QUEUE_SIZE, WRAPPER_CLASS) are not repeated: the protocol is an
# ELF module built with its own flags.
set(MESHPUNK_DEFINITIONS
    # Board select for the per-device backends (src/boards/board_pins.h).
    "BOARD_TDISPLAY_P4"
    # Arduino's String keeps its buffers in PSRAM (WString.h), as on the S3.
    "BOARD_HAS_PSRAM=1"
    # Serial = UART0 (HardwareSerial.h maps Serial to Serial0 when
    # ARDUINO_USB_CDC_ON_BOOT is unset): the board's USB-C data port is a
    # CH343 USB-UART bridge on UART0 (Windows: "USB-Enhanced-SERIAL CH343",
    # VID 1A86 / PID 55D3), the same port the IDF console and the bootloader
    # already write to. No USB-Serial-JTAG device enumerates from this board.
    # The radio object type (src/main.cpp) and the defaults CustomSX1262.h
    # programs in begin(); Phase 3 of the port plan adds the LR2021.
    "RADIO_CLASS=CustomSX1262"
    "LORA_FREQ=910.525"
    "LORA_BW=62.5"
    "LORA_SF=7"
    "LORA_TX_POWER=20"
    # TX power is RADIATED dBm everywhere in the UI (src/main.cpp caps the
    # setting at this); the SX1262 and the LR2021 both reach +22 dBm.
    "MAX_LORA_TX_POWER=22"
    # mesh_store.h: MSTORE_MAX_CHANNELS mirrors this value.
    "MAX_GROUP_CHANNELS=20"
    # BLE companion over the C6: NimBLE host on the P4 (sdkconfig.defaults,
    # Bluetooth block), transport class src/radio/punk_serial_ble.cpp. The
    # PIN and the name prefix belong to the companion protocol package
    # (modules/meshcore/build.ps1 BLE_PIN_CODE, mc_bleproto.cpp), not to the
    # firmware's defines.
    "BLE_COMPANION_ENABLED=1"
    # -1/-1 makes MeshCore's ESP32Board::begin() skip Wire.begin(): the
    # board layer owns the I2C buses.
    "PIN_BOARD_SDA=-1"
    "PIN_BOARD_SCL=-1")
idf_build_set_property(COMPILE_DEFINITIONS "${MESHPUNK_DEFINITIONS}" APPEND)

# Arduino's USB device classes need its arduino_tinyusb component
# (CONFIG_TINYUSB_ENABLED), which the core's IDF-component form does not
# carry; TinyUSB on this board is esp_tinyusb (drive mode,
# src/usb/usb_msc_dev.cpp). Two core files are gated on
# CONFIG_TINYUSB_MSC_ENABLED alone, a name esp_tinyusb's Kconfig defines
# too, so with drive mode built they would compile against the absent glue:
# they are left out of the core's build.
idf_component_get_property(arduino_dir espressif__arduino-esp32 COMPONENT_DIR)
idf_component_get_property(arduino_lib espressif__arduino-esp32 COMPONENT_LIB)
set_source_files_properties(
    "${arduino_dir}/cores/esp32/USBMSC.cpp"
    "${arduino_dir}/cores/esp32/FirmwareMSC.cpp"
    TARGET_DIRECTORY ${arduino_lib}
    PROPERTIES HEADER_FILE_ONLY TRUE)
