// board_pins.h — neutral names for the pins the common firmware still wires
// directly (shared SPI bus, chip selects, boot button, radio).
//
// Everything else is behind a subsystem backend (input/, display/, audio/,
// power/, gps/). This header exists for the wiring that legitimately belongs
// to the common code: the SPI bus the radio + SD + panel share, and the
// RadioLib Module constructor.
//
// Per-board block below; the board define comes from platformio.ini (the S3
// boards) or p4/CMakeLists.txt (the T-Display P4).

#pragma once

class SPIClass;

// The SPI bus the microSD slot lives on: the shared FSPI bus on the T-Deck,
// the display's HSPI bus on the Heltec kit. Defined by the board's display
// backend (which owns that bus's lifecycle); lazily begun on first call.
SPIClass& board_sd_spi(void);

#if defined(BOARD_TDECK)

#include "../utilities.h"
#include "../tdeck-pins.h"

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "tdeck"
// Display form of the same, used to build the default node name.
#define MESHPUNK_BOARD_LABEL "T-Deck"
// The SoC, reported to Lua via _device_caps().chip.
#define MESHPUNK_CHIP_NAME "esp32s3"
// Meshtastic HardwareModel enum value for this board (forwarded to the
// mtlite protocol for its NodeInfo broadcasts): T_DECK = 50 in
// meshtastic/protobufs mesh.proto.
#define MESHPUNK_MT_HW_MODEL 50

// LoRa radio (SX1262) — RadioLib Module pins
#define PIN_LORA_CS    RADIO_CS_PIN
#define PIN_LORA_DIO1  RADIO_DIO1_PIN
#define PIN_LORA_RST   RADIO_RST_PIN
#define PIN_LORA_BUSY  RADIO_BUSY_PIN

// Shared SPI bus (radio + SD + display)
#define PIN_SPI_SCK    BOARD_SPI_SCK
#define PIN_SPI_MISO   BOARD_SPI_MISO
#define PIN_SPI_MOSI   BOARD_SPI_MOSI

// Chip selects parked high at boot
#define PIN_SD_CS      BOARD_SDCARD_CS
#define PIN_TFT_CS     BOARD_TFT_CS

// Boot button
#define PIN_BOOT_BTN   BOARD_BOOT_PIN

#elif defined(BOARD_HELTEC_V4)

// Heltec WiFi LoRa 32 V4-R8 + Expansion Kit V2. Pin sources: Heltec's
// V4_Touch_TFT example, the Meshtastic heltec_v4 variant, and the vendored
// MeshCore variants/heltec_v4 — all three agree on the pins below.

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "heltec_v4"
// Display form of the same, used to build the default node name.
#define MESHPUNK_BOARD_LABEL "Heltec V4"
// The SoC, reported to Lua via _device_caps().chip.
#define MESHPUNK_CHIP_NAME "esp32s3"
// Meshtastic HardwareModel enum value for this board (forwarded to the
// mtlite protocol for its NodeInfo broadcasts): HELTEC_V4_R8 = 132 in
// meshtastic/protobufs mesh.proto.
#define MESHPUNK_MT_HW_MODEL 132

// LoRa radio (SX1262) — RadioLib Module pins. The radio has FSPI to itself
// on this board (the panel is on HSPI); the FEM control lines are separate
// build defines consumed by src/boards/heltec_fem.cpp.
// All hw-verified: CS/SCK/MOSI/MISO by the GetStatus probe (0x2A on 10/11),
// BUSY by the reset-pulse sampler (GPIO13 pulses high through POR, GPIO14
// stays low) — same assignments as the base V4 after all.
#define PIN_LORA_CS    8
#define PIN_LORA_DIO1  14
#define PIN_LORA_RST   12
#define PIN_LORA_BUSY  13

// FSPI bus (radio only; SD is probed on the display's HSPI bus)
#define PIN_SPI_SCK    9
#define PIN_SPI_MISO   11
#define PIN_SPI_MOSI   10

// microSD (Expansion Kit V2 slot): CS hw-confirmed by the boot probe
// (mounted a 32GB card); the slot shares the display's HSPI bus
// (SCK 16 / MOSI 15 / MISO 45 — see display_heltec.cpp). Parking this CS
// high at boot also prevents the card from ever seeing clocks CS-less
// (which wedges it until power-cycle). PIN_TFT_CS stays undefined: the
// panel CS belongs to the display backend.
#define PIN_SD_CS      3

// Buttons, both hw-probed 2026-08-11: USER = GPIO0 active LOW (shares the
// line with the mainboard PRG/boot button — one electrical input), IO =
// GPIO46 ACTIVE HIGH with an external pull-down (rides the R8's LED_Write
// line). GPIO35/36/37 must NEVER be driven on this board (octal PSRAM).
#define PIN_BOOT_BTN   0
#define PIN_IO_BTN     46

#elif defined(BOARD_WIO_L2)

// Seeed Wio Tracker L2 (Wio-S3 module: ESP32-S3R8 + SX1262). Pin sources:
// the Seeed wiki pin table, the Meshtastic seeed_wio_tracker_L2 variant and
// the wadamesh variants/wio_tracker_l2 port — all three agree on the pins
// below. Board-only lines (expander, I2C, display, backlight) live in
// src/boards/wio_l2_board.h.

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "wio_l2"
// Display form of the same, used to build the default node name.
#define MESHPUNK_BOARD_LABEL "Wio L2"
// The SoC, reported to Lua via _device_caps().chip.
#define MESHPUNK_CHIP_NAME "esp32s3"
// Meshtastic HardwareModel enum value for this board (forwarded to the
// mtlite protocol for its NodeInfo broadcasts): SEEED_WIO_TRACKER_L2 = 137
// (custom_meshtastic_hw_model in the Meshtastic variant).
#define MESHPUNK_MT_HW_MODEL 137

// LoRa radio (SX1262) — RadioLib Module pins. The radio has FSPI to itself:
// the panel is on its own QSPI bus (SPI3) and the microSD slot is SDMMC.
#define PIN_LORA_CS    21
#define PIN_LORA_DIO1  9
#define PIN_LORA_RST   7
#define PIN_LORA_BUSY  8

// FSPI bus (radio only)
#define PIN_SPI_SCK    4
#define PIN_SPI_MISO   5
#define PIN_SPI_MOSI   6

// PIN_SD_CS and PIN_TFT_CS stay undefined: the SD slot is 1-bit SDMMC
// (CLK 2 / CMD 3 / D0 1, not SPI) and the panel CS belongs to the display
// backend's own bus.

// USER button = GPIO0, active LOW (Meshtastic BUTTON_PIN 0). The WAKE
// button sits on the I/O expander (P00), not on a GPIO.
#define PIN_BOOT_BTN   0

// Microphone: the ES7243E ADC's data out is GPIO 15 into the speaker's I2S
// port (Meshtastic variant.h ADC_I2S_DIN; Seeed's pin list names it
// CODEC_I2S0_SDOUT). The ES8311's own ADC output is not wired.
#define PIN_MIC_I2S_DIN 15

#elif defined(BOARD_TDISPLAY_P4)

// LilyGo T-Display P4 v1 (ESP32-P4 + ESP32-C6 co-processor; LR2021 on this
// unit, SX1262 on the other SKU). Pin sources: LilyGo's
// lilygo_device_driver v1 config.h and the T-Display-P4 README hardware
// table, confirmed by the bring-up rungs in p4/main/
// (docs/P4_PORT_PLAN_2026-10-12.md §1). Board-only lines (XL9535 expander,
// the two I2C buses, the DSI panel, SDMMC) live with their backends.

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "tdisplay_p4"
// Display form of the same, used to build the default node name.
#define MESHPUNK_BOARD_LABEL "T-Display P4"
// The SoC, reported to Lua via _device_caps().chip.
#define MESHPUNK_CHIP_NAME "esp32p4"
// Meshtastic HardwareModel enum value for this board (forwarded to the
// mtlite protocol for its NodeInfo broadcasts): meshtastic/protobufs
// mesh.proto has no T-Display P4 entry; PRIVATE_HW = 255 is its value for
// hardware outside the enum.
#define MESHPUNK_MT_HW_MODEL 255

// LoRa radio — RadioLib Module pins. CS and BUSY are GPIOs. The radio's
// RESET and DIO1 are XL9535 expander lines (IO16, IO17), which RadioLib's
// Module cannot drive, so both are -1 (RADIOLIB_NC, which its Arduino HAL
// skips): the board layer resets the chip through the expander, and the
// polled radio HAL reads the chip's IRQ register (radio/punk_polled_radio.h)
// rather than a DIO1 line. Phase 3 of the port plan wires the chip.
#define PIN_LORA_CS    24
#define PIN_LORA_DIO1  -1
#define PIN_LORA_RST   -1
#define PIN_LORA_BUSY  6

// Radio SPI bus (the radio has it to itself: the panel is MIPI-DSI and the
// microSD slot is SDMMC)
#define PIN_SPI_SCK    2
#define PIN_SPI_MISO   4
#define PIN_SPI_MOSI   3

// PIN_SD_CS and PIN_TFT_CS stay undefined: SDMMC slot 0 and a DSI panel.

// BOOT button = GPIO35, active LOW, the board's only button.
#define PIN_BOOT_BTN   35

// Microphone: the ES8311 codec's own ADC; the codec's data out is GPIO 11
// (LilyGo's v1 config.h, es8311 kAdcData) into the speaker's I2S port.
#define PIN_MIC_I2S_DIN 11

#else
#error "No board selected: define BOARD_<NAME> (platformio.ini build_flags, or p4/CMakeLists.txt for the T-Display P4)"
#endif
