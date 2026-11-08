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
    # BLE companion off until it runs over the C6 (port plan §6). The S3's
    # BLE_PIN_CODE / BLE_NAME_PREFIX / BLE_DEBUG_LOGGING are read only by the
    # companion sources this define compiles out, so they are not set here.
    "BLE_COMPANION_ENABLED=0"
    # -1/-1 makes MeshCore's ESP32Board::begin() skip Wire.begin(): the
    # board layer owns the I2C buses.
    "PIN_BOARD_SDA=-1"
    "PIN_BOARD_SCL=-1")
idf_build_set_property(COMPILE_DEFINITIONS "${MESHPUNK_DEFINITIONS}" APPEND)
