// rtc_tdisplay_p4.cpp — LilyGo T-Display P4 hardware clock backend
// (contract: rtc_dev.h): NXP PCF8563 at 0x51 on the board layer's I2C bus
// (LilyGo's v1 config.h: `pcf8563` on the same port as the XL9535 and the
// gauge). Registers 0x02..0x08 hold BCD seconds, minutes, hours, day of
// month, weekday, month and two-digit year; bit 7 of the seconds register
// is VL, which the chip sets when its supply dropped below the clock-
// integrity threshold and which clears when the register is written;
// Control_status_1 bit 5 (STOP) halts the clock. The century bit (month
// register bit 7) is written 0 and ignored on read: the two-digit year
// alone places the time in 2000-2099. Times are UTC both ways; tiers and
// the plausibility floor are meshpunk_set_clock()'s (main.cpp).

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include "driver/i2c_master.h"

#include "rtc_dev.h"
#include "../boards/tdisplay_p4_board.h"
#include "../meshpunk_sync.h"   // SLog

#define PCF8563_ADDR           0x51
#define PCF8563_REG_CTRL1      0x00   // Control_status_1
#define PCF8563_REG_SECONDS    0x02   // VL_seconds
#define PCF8563_CTRL1_STOP     0x20
#define PCF8563_VL             0x80

static i2c_master_dev_handle_t s_rtc = NULL;
static bool s_add_failed = false;

// The device is added to the board layer's bus on first use (the bus comes
// up in power_dev_init(), before any caller of this file runs).
static i2c_master_dev_handle_t rtc_handle() {
  if (s_rtc || s_add_failed) return s_rtc;
  if (!tdp4_board_ready()) return NULL;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = PCF8563_ADDR;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(tdp4_i2c_bus(), &cfg, &s_rtc) != ESP_OK) {
    s_rtc = NULL;
    s_add_failed = true;
    SLog.println("[CLOCK] rtc: FAIL PCF8563 device add");
  }
  return s_rtc;
}

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Proleptic Gregorian calendar <-> days since 1970-01-01 (Howard Hinnant's
// days_from_civil / civil_from_days).
static int32_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)yoe + era * 400 + (*m <= 2);
}

bool rtc_dev_read(uint32_t* epoch) {
  i2c_master_dev_handle_t dev = rtc_handle();
  if (!dev || !epoch) return false;
  uint8_t reg = PCF8563_REG_CTRL1;
  uint8_t r[9];   // Control_status_1 .. Years
  if (i2c_master_transmit_receive(dev, &reg, 1, r, sizeof(r), 100) != ESP_OK) {
    SLog.println("[CLOCK] rtc: PCF8563 at 0x51 not answering");
    return false;
  }
  if (r[0] & PCF8563_CTRL1_STOP) {
    SLog.println("[CLOCK] rtc: PCF8563 clock halted (STOP set), time not used");
    return false;
  }
  if (r[2] & PCF8563_VL) {
    SLog.println("[CLOCK] rtc: PCF8563 voltage-low flag set, stored time not trusted");
    return false;
  }
  unsigned sec  = bcd2bin(r[2] & 0x7F);
  unsigned min  = bcd2bin(r[3] & 0x7F);
  unsigned hour = bcd2bin(r[4] & 0x3F);
  unsigned day  = bcd2bin(r[5] & 0x3F);
  unsigned mon  = bcd2bin(r[7] & 0x1F);
  unsigned year = 2000 + bcd2bin(r[8]);
  if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12) {
    SLog.printf("[CLOCK] rtc: PCF8563 registers out of range: %02X %02X %02X %02X %02X %02X %02X\n",
                r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
    return false;
  }
  int32_t days = days_from_civil((int)year, mon, day);
  *epoch = (uint32_t)days * 86400UL + hour * 3600UL + min * 60UL + sec;
  SLog.printf("[CLOCK] rtc: PCF8563 reads %04u-%02u-%02u %02u:%02u:%02u UTC = %u\n",
              year, mon, day, hour, min, sec, (unsigned)*epoch);
  return true;
}

bool rtc_dev_write(uint32_t epoch) {
  i2c_master_dev_handle_t dev = rtc_handle();
  if (!dev) return false;
  int32_t  days = (int32_t)(epoch / 86400UL);
  uint32_t rem  = epoch % 86400UL;
  int y; unsigned m, d;
  civil_from_days(days, &y, &m, &d);
  if (y < 2000 || y > 2099) {
    SLog.printf("[CLOCK] rtc: %u is outside the PCF8563's 2000-2099 range, not written\n",
                (unsigned)epoch);
    return false;
  }
  // Control_status_1 = 0: normal mode, STOP clear. Then the seven time
  // registers in one auto-incrementing write; seconds bit 7 = 0 clears VL.
  uint8_t ctrl[2] = { PCF8563_REG_CTRL1, 0x00 };
  uint8_t w[8];
  w[0] = PCF8563_REG_SECONDS;
  w[1] = bin2bcd((uint8_t)(rem % 60));
  w[2] = bin2bcd((uint8_t)((rem / 60) % 60));
  w[3] = bin2bcd((uint8_t)(rem / 3600));
  w[4] = bin2bcd((uint8_t)d);
  w[5] = (uint8_t)((days + 4) % 7);   // weekday register 0 = Sunday; 1970-01-01 was a Thursday
  w[6] = bin2bcd((uint8_t)m);         // century bit 0
  w[7] = bin2bcd((uint8_t)(y - 2000));
  if (i2c_master_transmit(dev, ctrl, sizeof(ctrl), 100) != ESP_OK ||
      i2c_master_transmit(dev, w, sizeof(w), 100) != ESP_OK) {
    SLog.println("[CLOCK] rtc: PCF8563 write failed");
    return false;
  }
  SLog.printf("[CLOCK] rtc: PCF8563 set to %04d-%02u-%02u %02u:%02u:%02u UTC\n",
              y, m, d, (unsigned)(rem / 3600), (unsigned)((rem / 60) % 60), (unsigned)(rem % 60));
  return true;
}

#endif // BOARD_TDISPLAY_P4
