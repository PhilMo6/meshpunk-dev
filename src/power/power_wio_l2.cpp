// power_wio_l2.cpp — Seeed Wio Tracker L2 power/battery backend
// (contract: power_dev.h).
//
// Every switched rail on this board is a PCA9555 expander output (LCD,
// GNSS, SD, Grove, audio PA, battery divider enable — see
// src/boards/wio_l2_board.h). power_dev_init() runs the board bring-up
// (wio_l2_board_init) that powers and resets them in order.
//
// Battery: ADS1115 (I2C 0x48) AIN0 behind a 1:2 divider — the Meshtastic
// ADS1115BatteryLevel reading: GAIN_ONE (+/-4.096 V), 860 SPS, single-ended
// AIN0, volts x 2. The divider enable (expander P17) is left on by the
// bring-up.
//
// Wake from power-off: the USER button (GPIO0, active LOW) via ext0. The
// WAKE button cannot do it — it sits on the expander, whose INT (GPIO45) is
// not an RTC pad.

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>      // esp_reset_reason

#include "power_dev.h"
#include "../boards/board_pins.h"     // PIN_BOOT_BTN
#include "../boards/wio_l2_board.h"
#include "../meshpunk_sync.h"         // SLog

void power_dev_init(void) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // Waking from power-off: release the RTC routing the shutdown path armed
    // on the wake button, or input_dev_preinit's pull-up does nothing
    // against the RTC-routed pad.
    rtc_gpio_deinit((gpio_num_t)PIN_BOOT_BTN);
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  }

  // I2C bus, expander probe, then the rail/reset sequence. Logs its own
  // FAIL line; everything downstream that needs the expander logs too.
  wio_l2_board_init();
}

// ── Battery (ADS1115) ──────────────────────────────────────────────────────
#define ADS_REG_CONV    0x00
#define ADS_REG_CONFIG  0x01
// OS=1 (start a conversion), MUX=100 (AIN0 vs GND), PGA=001 (+/-4.096 V),
// MODE=1 (single shot), DR=111 (860 SPS), comparator off (COMP_QUE=11).
#define ADS_CFG_AIN0_SINGLE  0xC3E3
#define ADS_SAMPLES          4

static bool ads_write_config(uint16_t cfg) {
  Wire.beginTransmission(WIO_L2_BATT_ADC_ADDR);
  Wire.write(ADS_REG_CONFIG);
  Wire.write((uint8_t)(cfg >> 8));
  Wire.write((uint8_t)(cfg & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool ads_read_reg(uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(WIO_L2_BATT_ADC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WIO_L2_BATT_ADC_ADDR, 2) != 2) return false;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  *out = (uint16_t)((hi << 8) | lo);
  return true;
}

// One single-shot AIN0 conversion. A conversion takes 1.2 ms at 860 SPS;
// the config register's OS bit reads 1 once it is done.
static bool ads_read_ain0(int16_t* raw) {
  if (!ads_write_config(ADS_CFG_AIN0_SINGLE)) return false;
  for (int i = 0; i < 10; i++) {
    delay(1);
    uint16_t cfg = 0;
    if (!ads_read_reg(ADS_REG_CONFIG, &cfg)) return false;
    if (cfg & 0x8000) {
      uint16_t v = 0;
      if (!ads_read_reg(ADS_REG_CONV, &v)) return false;
      *raw = (int16_t)v;
      return true;
    }
  }
  return false;
}

static bool s_batt_fail_logged = false;

uint16_t power_dev_battery_mv(void) {
  int32_t sum = 0;
  for (int i = 0; i < ADS_SAMPLES; i++) {
    int16_t raw = 0;
    if (!ads_read_ain0(&raw)) {
      if (!s_batt_fail_logged) {
        s_batt_fail_logged = true;
        SLog.printf("[BATT] FAIL: ADS1115 at 0x%02X not answering\n",
                    WIO_L2_BATT_ADC_ADDR);
      }
      return 0;
    }
    sum += raw;
  }
  // 4.096 V over 32768 counts = 0.125 mV per count at the pin; x2 for the
  // divider = 0.25 mV per count.
  int32_t mv = (sum / ADS_SAMPLES) / 4;
  if (mv < 0) mv = 0;
  return (uint16_t)mv;
}

// ── Power off ──────────────────────────────────────────────────────────────

void power_dev_shutdown(void) {
  // Switched rails OFF, and the panel/touch control lines LOW so nothing
  // back-feeds the unpowered parts. The expander holds its output register
  // while powered, so these levels persist through deep sleep.
  static const uint8_t kOff[] = {
    WIO_L2_EXP_LCD_PWR,  WIO_L2_EXP_LCD_RST,   WIO_L2_EXP_LCD_CTRL,
    WIO_L2_EXP_TP_RST,   WIO_L2_EXP_GNSS_PWR,  WIO_L2_EXP_SD_PWR,
    WIO_L2_EXP_GROVE_PWR, WIO_L2_EXP_AUDIO_PA, WIO_L2_EXP_USER_LED,
    WIO_L2_EXP_USB_OTG,  WIO_L2_EXP_BAT_SENSE,
  };
  for (uint8_t bit : kOff) wio_l2_exp_set(bit, false);

  // Wake: USER button (GPIO0, active LOW) — the T-Deck/Heltec recipe; the
  // RTC-domain pull-up must persist or the pad floats LOW and instantly
  // re-wakes.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  rtc_gpio_init((gpio_num_t)PIN_BOOT_BTN);
  rtc_gpio_set_direction((gpio_num_t)PIN_BOOT_BTN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BOOT_BTN);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BOOT_BTN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT_BTN, 0);

  esp_deep_sleep_start();   // never returns; a press reboots
}

// Standby keeps the LCD rail (panel sleeping, frame memory kept) and cuts
// only the GNSS rail, as on the Heltec. The L76K cold-boots back to its
// factory 9600 NMEA on exit, the rate the firmware's baud probe locked at
// boot. Expander outputs are external state: nothing to hold across light
// sleep.
void power_dev_standby_enter(void) {
  wio_l2_exp_set(WIO_L2_EXP_GNSS_PWR, false);
}

void power_dev_standby_exit(void) {
  wio_l2_exp_set(WIO_L2_EXP_GNSS_PWR, true);
}

#endif // BOARD_WIO_L2
