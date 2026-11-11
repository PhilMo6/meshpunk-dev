// power_tdisplay_p4.cpp — LilyGo T-Display P4 power/battery backend
// (contract: power_dev.h).
//
// Board facts (docs/P4_PORT_PLAN_2026-10-12.md §1, LilyGo's v1 config.h):
// every switched rail and reset is an XL9535 expander line; the board layer
// (src/boards/tdisplay_p4_board.cpp) owns the bus, the expander and the
// power-up sequence, and comes up from power_dev_init() here — before the
// display backend runs (setup() order), as on the Wio L2. The battery
// gauge is a TI BQ27220 at 0x55 on the same I2C bus (LilyGo's v1 config:
// `bq27220` on I2C port 1), powered from the cell it measures, so it
// answers only with a battery connected; its Voltage() standard command
// (0x08) returns the cell voltage in mV. Off: GPIO35 (BOOT) is not an LP
// IO — the P4's deep-sleep GPIO wake covers GPIO0-15 (soc_caps.h
// SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK) — so the BOOT button cannot wake
// the chip from deep sleep, and power off is a light sleep instead
// (power_dev_shutdown below).

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <esp_sleep.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "power_dev.h"
#include "../boards/board_pins.h"          // PIN_BOOT_BTN
#include "../boards/tdisplay_p4_board.h"
#include "../meshpunk_sync.h"   // SLog

#define BQ27220_ADDR          0x55
#define BQ27220_CMD_VOLTAGE   0x08   // Voltage(): mV, 16-bit little-endian
#define BQ27220_CMD_CURRENT   0x0C   // Current(): mA, signed 16-bit little-endian

static i2c_master_dev_handle_t s_gauge = NULL;

static bool gauge_read_u16(uint8_t cmd, uint16_t* out) {
  uint8_t b[2];
  if (!s_gauge) return false;
  if (i2c_master_transmit_receive(s_gauge, &cmd, 1, b, sizeof(b), 100) != ESP_OK) return false;
  *out = (uint16_t)(b[0] | (b[1] << 8));
  return true;
}

void power_dev_init(void) {
  if (!tdp4_board_init()) {
    SLog.println("[P4] power backend: board layer did not come up (its FAIL line is above)");
    return;
  }
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = BQ27220_ADDR;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(tdp4_i2c_bus(), &cfg, &s_gauge) != ESP_OK) {
    s_gauge = NULL;
    SLog.println("[P4] FAIL: battery gauge device add");
    return;
  }
  uint16_t mv = 0;
  if (gauge_read_u16(BQ27220_CMD_VOLTAGE, &mv)) {
    SLog.printf("[P4] BQ27220 gauge: %u mV\n", (unsigned)mv);
  } else {
    SLog.println("[P4] BQ27220 gauge at 0x55 not answering (no battery connected, or no gauge)");
  }
}

uint16_t power_dev_battery_mv(void) {
  uint16_t mv = 0;
  return gauge_read_u16(BQ27220_CMD_VOLTAGE, &mv) ? mv : 0;
}

// Off: the board's remaining loads parked, then a light sleep with only the
// BOOT button armed; a press restarts the chip, so waking is a fresh boot as
// after the other boards' deep sleep (reset reason: software). By the time
// this runs, system_shutdown() (main.cpp) has flushed the stores, stopped
// BLE and WiFi (esp-hosted deinitialised), slept the radio, unmounted the
// card, parked the GPS (IO11) and put the panel in sleep-in with its DSI
// link down. The next boot's board init sets every expander line again.
void power_dev_shutdown(void) {
  tdp4_exp_set(TDP4_IO_TOUCH_RST, false);   // touch controller in reset
  tdp4_exp_set(TDP4_IO_C6_EN, false);       // C6 off
  tdp4_exp_set(TDP4_IO_5V_EN, false);       // audio power (LilyGo kAudioPowerEn)
  tdp4_exp_set(TDP4_IO_SD_EN, true);        // card slot power off (active LOW)

  // BOOT is also the select key: the press that chose Off must be over
  // before the button is armed.
  while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
  delay(50);

  // The click ISR's interrupt goes off first: never level-arm a pin whose
  // interrupt is enabled. The pad keeps its runtime pull-up in light sleep.
  gpio_intr_disable((gpio_num_t)PIN_BOOT_BTN);
  gpio_sleep_sel_dis((gpio_num_t)PIN_BOOT_BTN);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_enable((gpio_num_t)PIN_BOOT_BTN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  SLog.println("[P4] power off: light sleep, BOOT (GPIO35) restarts");
  delay(20);   // let the serial line drain
  while (true) {
    esp_light_sleep_start();
    if (digitalRead(PIN_BOOT_BTN) == LOW) break;   // a press, not a glitch
  }
  esp_restart();   // never returns
}

// ── Standby (light sleep) ──────────────────────────────────────────────────
// Wake sources: the BOOT button (GPIO35, armed by the shared standby code),
// the timer, and the radio. DIO1 (SX1262 DIO1; LR2021 DIO11, see
// radio_chip_lr2021.cpp) is expander input IO17, so a received packet
// reaches the P4 through the expander's INT line (GPIO5, active LOW while an
// input differs from its last read). The other expander inputs are the touch,
// IMU and RTC interrupt lines. The touch controller is held in reset for the
// standby with its INT line (IO4) driven LOW, so touches cannot assert INT;
// the IMU is never started and the RTC's alarm and timer are unused.

void power_dev_standby_enter(void) {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << TDP4_EXP_INT_GPIO;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  gpio_sleep_sel_dis((gpio_num_t)TDP4_EXP_INT_GPIO);   // runtime config kept through light sleep
  // The GT9895 samples its INT line only as reset releases: reset first,
  // then the line becomes an output at LOW.
  tdp4_exp_set(TDP4_IO_TOUCH_RST, false);
  tdp4_exp_set(TDP4_IO_TOUCH_INT, false);
  tdp4_exp_dir(TDP4_IO_TOUCH_INT, false);
}

// The touch controller comes back in its power-on order: INT an input
// again, then reset released. It boots (touch_init() allows 100 ms) while
// standby_run() wakes the display and repaints; the first touch poll runs
// after standby_run() returns.
void power_dev_standby_exit(void) {
  tdp4_exp_dir(TDP4_IO_TOUCH_INT, true);
  tdp4_exp_set(TDP4_IO_TOUCH_INT, true);   // output latch back to the base state
  tdp4_exp_set(TDP4_IO_TOUCH_RST, true);
}

// Reading the input ports releases INT. DIO1 already HIGH at that read means
// a received packet is waiting for the mesh task: no sleep this round.
bool power_dev_standby_arm(bool radio) {
  if (!radio) return true;
  uint16_t in = 0;
  if (!tdp4_exp_read_inputs(&in)) {
    static uint32_t fails = 0;
    if (++fails <= 5) SLog.printf("[P4] standby: expander input read FAILED (%lu)\n", (unsigned long)fails);
  } else if (in & (1u << (8 + TDP4_IO_RADIO_DIO1 - 10))) {
    return false;
  }
  gpio_wakeup_enable((gpio_num_t)TDP4_EXP_INT_GPIO, GPIO_INTR_LOW_LEVEL);
  return true;
}

void power_dev_standby_disarm(bool radio) {
  if (!radio) return;
  gpio_wakeup_disable((gpio_num_t)TDP4_EXP_INT_GPIO);
}

// USB host port power. 5 V for an accessory on the USB-A host socket comes
// from a TPS2051C fed by VCC_5V, which only a cable on either USB-C port
// provides: the board has no boost from the cell (schematic, power sheet).
// The gauge's current sign tells the states apart: on the cell alone the
// current is negative; charging or full on a cable it is zero or positive.
bool power_dev_usb_port_power(void) {
  uint16_t raw = 0;
  if (!gauge_read_u16(BQ27220_CMD_CURRENT, &raw)) return false;
  return (int16_t)raw >= 0;
}

bool power_dev_usb_host_begin(void) { return power_dev_usb_port_power(); }
void power_dev_usb_host_tick(void) {}
void power_dev_usb_host_end(void) {}

#endif // BOARD_TDISPLAY_P4
