// tdisplay_p4_board.h — LilyGo T-Display P4 (v1) board layer: the shared
// I2C bus, the XL9535 I/O expander, the 3.3 V peripheral LDO rail and the
// power-up sequence. Every subsystem backend for this board (display,
// input, SD, radio, GPS, power) reaches its expander-controlled lines
// through here.
//
// Sources: LilyGo's lilygo_device_driver t_display_p4/v1 config and the
// wadamesh variants/tdisplay_p4 port (Xl9535.cpp, power-up order), both
// hardware-tested on this board by the bring-up ladder in p4/main/
// (rung2_display.c runs the same sequence).

#pragma once

#if defined(BOARD_TDISPLAY_P4)

#include <stdint.h>
#include "driver/i2c_master.h"

// ── Shared I2C bus (expander, touch, RTC, gauge, codec, IMU) ───────────────
#define TDP4_I2C_PORT        0
#define TDP4_I2C_SDA         7
#define TDP4_I2C_SCL         8

// ── XL9535 I/O expander (PCA9535-compatible) ────────────────────────────────
#define TDP4_EXP_ADDR        0x20
// The expander's INT output (active LOW while an input line differs from its
// last read; reading the input ports releases it) on P4 GPIO5, an LP IO
// (LilyGo v1 config: xl9535::kInt).
#define TDP4_EXP_INT_GPIO    5

// Line numbers: IO0-IO7 = port 0 bits 0-7, IO10-IO17 = port 1 bits 0-7
// (no IO8/IO9 exists — the wadamesh/LilyGo numbering).
#define TDP4_IO_3V3_EN       0
#define TDP4_IO_RF_SWITCH    1
#define TDP4_IO_SCREEN_RST   2
#define TDP4_IO_TOUCH_RST    3
#define TDP4_IO_TOUCH_INT    4    // input
#define TDP4_IO_ETH_RST      5    // IP101 Ethernet PHY reset, active LOW (LilyGo: kEthernetRst)
#define TDP4_IO_5V_EN        6
#define TDP4_IO_SENSOR_INT   7    // input
#define TDP4_IO_VCCA_EN      10   // LilyGo: kUsbPhyPowerEn
#define TDP4_IO_GPS_WAKE     11   // high = L76K running, low = parked (LilyGo's L76k::Sleep)
#define TDP4_IO_RTC_INT      12   // input
#define TDP4_IO_C6_EN        14
#define TDP4_IO_SD_EN        15   // active LOW
#define TDP4_IO_RADIO_RST    16   // SX1262 and LR2021 SKUs alike
#define TDP4_IO_RADIO_DIO1   17   // input

// ── Panel geometry shared by the display and input backends ────────────────
// The RM69A10 is 568x1232 portrait-native. The UI renders at a logical
// 616x284 native landscape (plan D3): each logical pixel is a 2x2 panel
// block; logical x runs along the panel's long axis from its bottom (panel
// row 1231 = logical x 0) and logical y runs across the short axis from the
// panel's left edge (panel column 0 = logical y 0). That is the panel held
// with its top edge to the viewer's right — the keyboard connector's pogo
// pins facing down; orientation 2 (display_dev.h: landscape flipped) is the
// other way round.
#define TDP4_PANEL_W         568
#define TDP4_PANEL_H         1232
#define TDP4_UI_W            616  // = TDP4_PANEL_H / 2
#define TDP4_UI_H            284  // = TDP4_PANEL_W / 2

// Even (top-left) corner of the 2x2 panel block of native-landscape pixel
// (lx, ly): the block is columns px, px+1 and rows py, py+1.
static inline void tdp4_ui_to_panel(int lx, int ly, int* px, int* py) {
  *px = 2 * ly;
  *py = TDP4_PANEL_H - 2 - 2 * lx;
}

// Bring the board up: I2C bus clear + bus create, expander probe, the
// expander base state, the power-rail cycle, the card/radio/touch lines,
// the C6 power cycle, esp-hosted's C6 reset callback (the EN line is
// expander IO14), then the 3.3 V peripheral LDO (channel 4). Runs once;
// later calls return the first result. Returns false (after logging which
// step failed) when the expander does not answer — nothing downstream works
// without it.
bool tdp4_board_init(void);

// True once tdp4_board_init() has completed successfully.
bool tdp4_board_ready(void);

// The shared I2C bus; NULL before a successful init.
i2c_master_bus_handle_t tdp4_i2c_bus(void);

// Drive one expander line as an output; false = the I2C write failed.
bool tdp4_exp_set(uint8_t io, bool level);

// Read one expander line's input level; false = the I2C read failed.
bool tdp4_exp_get(uint8_t io, bool* level);

// Set one expander line's direction (true = input); false = the I2C write
// failed. tdp4_board_init() sets the base directions.
bool tdp4_exp_dir(uint8_t io, bool input);

// Read both input ports: port 0 (IO0-IO7) in bits 0-7, port 1 (IO10-IO17)
// in bits 8-15. Reading them releases the INT line. false = an I2C read
// failed.
bool tdp4_exp_read_inputs(uint16_t* in);

#endif // BOARD_TDISPLAY_P4
