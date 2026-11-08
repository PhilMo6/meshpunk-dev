// tdisplay_p4_board.cpp — LilyGo T-Display P4 board layer (see
// tdisplay_p4_board.h).

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_rom_sys.h"
#include "esp_hosted_transport_config.h"   // esp_hosted_sdio_set_reset_callback (MESHPUNK patch)

#include "tdisplay_p4_board.h"
#include "../meshpunk_sync.h"   // SLog

// XL9535 registers, one per port (port 0 then port 1): input 0x00/0x01,
// output 0x02/0x03, configuration 0x06/0x07 (bit 1 = input).
#define XL_REG_IN0   0x00
#define XL_REG_OUT0  0x02
#define XL_REG_CFG0  0x06

static i2c_master_bus_handle_t  s_bus        = NULL;
static i2c_master_dev_handle_t  s_exp        = NULL;
static esp_ldo_channel_handle_t s_periph_ldo = NULL;
static uint8_t s_out[2];            // output-register shadows, port 0 / port 1
static uint8_t s_cfg[2];            // configuration-register shadows (bit 1 = input)
static bool    s_ready = false;
static bool    s_done  = false;     // init ran, whatever its result

static inline uint8_t io_port(uint8_t io) { return io >= 10 ? 1 : 0; }
static inline uint8_t io_bit(uint8_t io)  { return io >= 10 ? (uint8_t)(io - 10) : io; }

static bool xl_write(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = { reg, val };
  return i2c_master_transmit(s_exp, buf, sizeof(buf), 100) == ESP_OK;
}

static bool xl_read(uint8_t reg, uint8_t* val) {
  return i2c_master_transmit_receive(s_exp, &reg, 1, val, 1, 100) == ESP_OK;
}

bool tdp4_exp_set(uint8_t io, bool level) {
  if (!s_exp) return false;
  uint8_t p = io_port(io);
  uint8_t m = (uint8_t)(1u << io_bit(io));
  if (level) s_out[p] |= m;
  else       s_out[p] &= (uint8_t)~m;
  return xl_write((uint8_t)(XL_REG_OUT0 + p), s_out[p]);
}

bool tdp4_exp_get(uint8_t io, bool* level) {
  if (!s_exp) return false;
  uint8_t v = 0;
  if (!xl_read((uint8_t)(XL_REG_IN0 + io_port(io)), &v)) return false;
  if (level) *level = (v >> io_bit(io)) & 1u;
  return true;
}

bool tdp4_exp_dir(uint8_t io, bool input) {
  if (!s_exp) return false;
  uint8_t p = io_port(io);
  uint8_t m = (uint8_t)(1u << io_bit(io));
  if (input) s_cfg[p] |= m;
  else       s_cfg[p] &= (uint8_t)~m;
  return xl_write((uint8_t)(XL_REG_CFG0 + p), s_cfg[p]);
}

bool tdp4_exp_read_inputs(uint16_t* in) {
  if (!s_exp) return false;
  uint8_t v0 = 0, v1 = 0;
  if (!xl_read(XL_REG_IN0, &v0) || !xl_read(XL_REG_IN0 + 1, &v1)) return false;
  if (in) *in = (uint16_t)(v0 | (v1 << 8));
  return true;
}

bool tdp4_board_ready(void) { return s_ready; }

i2c_master_bus_handle_t tdp4_i2c_bus(void) { return s_bus; }

// esp-hosted's co-processor reset (libraries/esp_hosted, MESHPUNK patch):
// `level` is the C6 EN level, high = run. The host library pulses it inside
// esp_hosted_connect_to_slave(), which Arduino's esp32-hal-hosted.c runs at
// the first WiFi or BLE use; without a callback it would pulse its reset
// GPIO, 54 on the P4 variant, a user-header pin on this board.
static esp_err_t c6_reset_level(void* arg, bool level) {
  (void)arg;
  return tdp4_exp_set(TDP4_IO_C6_EN, level) ? ESP_OK : ESP_FAIL;
}

// I2C-spec bus clear before the bus driver takes the pins: a chip reset that
// lands inside an I2C transaction (an upload reset during a touch poll)
// leaves the addressed slave driving SDA low until it sees clock edges, and
// only a power cycle would otherwise end that. Up to nine SCL pulses let the
// slave finish its byte; the START + STOP afterwards releases every slave's
// address decoder.
static void bus_clear(void) {
  gpio_config_t od = {};
  od.pin_bit_mask = (1ULL << TDP4_I2C_SDA) | (1ULL << TDP4_I2C_SCL);
  od.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  gpio_config(&od);
  gpio_set_level((gpio_num_t)TDP4_I2C_SDA, 1);
  gpio_set_level((gpio_num_t)TDP4_I2C_SCL, 1);
  esp_rom_delay_us(10);
  if (!gpio_get_level((gpio_num_t)TDP4_I2C_SDA)) {
    int pulses = 0;
    while (pulses < 9 && !gpio_get_level((gpio_num_t)TDP4_I2C_SDA)) {
      gpio_set_level((gpio_num_t)TDP4_I2C_SCL, 0);
      esp_rom_delay_us(5);
      gpio_set_level((gpio_num_t)TDP4_I2C_SCL, 1);
      esp_rom_delay_us(5);
      pulses++;
    }
    SLog.printf("[P4] I2C bus clear: SDA was held low, %s after %d pulses\n",
                gpio_get_level((gpio_num_t)TDP4_I2C_SDA) ? "released" : "STILL HELD",
                pulses);
  }
  gpio_set_level((gpio_num_t)TDP4_I2C_SDA, 0);
  esp_rom_delay_us(5);
  gpio_set_level((gpio_num_t)TDP4_I2C_SDA, 1);
  esp_rom_delay_us(5);
}

// Power-up sequence = the wadamesh powerOnSequence (200 ms phases, final
// states VCCA LOW, 5V HIGH, 3V3 LOW), then the card, radio and touch lines,
// then a C6 power cycle — the order rung 2 ran before every panel bring-up
// on this board.
bool tdp4_board_init(void) {
  if (s_done) return s_ready;
  s_done = true;

  bus_clear();

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = TDP4_I2C_PORT;
  bus_cfg.sda_io_num = (gpio_num_t)TDP4_I2C_SDA;
  bus_cfg.scl_io_num = (gpio_num_t)TDP4_I2C_SCL;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
    s_bus = NULL;
    SLog.printf("[P4] FAIL: I2C master bus create (port %d, SDA %d / SCL %d)\n",
                TDP4_I2C_PORT, TDP4_I2C_SDA, TDP4_I2C_SCL);
    return false;
  }
  if (i2c_master_probe(s_bus, TDP4_EXP_ADDR, 100) != ESP_OK) {
    SLog.printf("[P4] FAIL: XL9535 at 0x%02X not answering on SDA %d / SCL %d\n",
                TDP4_EXP_ADDR, TDP4_I2C_SDA, TDP4_I2C_SCL);
    return false;
  }
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = TDP4_EXP_ADDR;
  dev_cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_exp) != ESP_OK) {
    s_exp = NULL;
    SLog.println("[P4] FAIL: expander device add");
    return false;
  }

  // Base state (wadamesh Xl9535::begin): every line an output at HIGH, then
  // the four interrupt lines back to inputs. The exception is the Ethernet
  // PHY's reset (IO5), LOW: the firmware does not use Ethernet, and the PHY
  // stays in reset.
  s_out[0] = (uint8_t)~(1u << TDP4_IO_ETH_RST);
  s_out[1] = 0xFF;
  s_cfg[0] = (uint8_t)((1u << TDP4_IO_TOUCH_INT) | (1u << TDP4_IO_SENSOR_INT));
  s_cfg[1] = (uint8_t)((1u << (TDP4_IO_RTC_INT - 10)) | (1u << (TDP4_IO_RADIO_DIO1 - 10)));
  bool ok = xl_write(XL_REG_OUT0, s_out[0]) &&
            xl_write(XL_REG_OUT0 + 1, s_out[1]) &&
            xl_write(XL_REG_CFG0, s_cfg[0]) &&
            xl_write(XL_REG_CFG0 + 1, s_cfg[1]);
  if (!ok) {
    SLog.println("[P4] FAIL: expander base-state writes");
    return false;
  }

  // Power rails. The GPS wake line is driven high = receiver awake: LilyGo's
  // L76K driver (cpp_bus_driver l76k.cpp, Init -> Sleep(false)) writes the
  // line to !sleep, so 1 runs the receiver and 0 parks it (their board init
  // writes 0 only as the pre-init state). IO10 (wadamesh "VCCA", LilyGo
  // kUsbPhyPowerEn) is driven high as LilyGo's init does: their comment has
  // the P4 reaching its low-power state only with that rail on.
  ok = tdp4_exp_set(TDP4_IO_GPS_WAKE, true) &&
       tdp4_exp_set(TDP4_IO_VCCA_EN, true) &&
       tdp4_exp_set(TDP4_IO_5V_EN, true);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_5V_EN, false);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_5V_EN, true) &&
             tdp4_exp_set(TDP4_IO_3V3_EN, false);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_3V3_EN, true);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_3V3_EN, false);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_SD_EN, false) &&       // active low: slot powered
             tdp4_exp_set(TDP4_IO_RADIO_RST, true) &&
             tdp4_exp_set(TDP4_IO_TOUCH_RST, true);
  // C6 power cycle: every boot starts the co-processor fresh.
  ok = ok && tdp4_exp_set(TDP4_IO_C6_EN, true);
  delay(100);
  ok = ok && tdp4_exp_set(TDP4_IO_C6_EN, false);
  delay(100);
  ok = ok && tdp4_exp_set(TDP4_IO_C6_EN, true);
  delay(100);
  if (!ok) {
    SLog.println("[P4] FAIL: expander write during the power-up sequence");
    return false;
  }

  // The C6 reset path for esp-hosted, registered before any WiFi or BLE use
  // can start the host library.
  if (esp_hosted_sdio_set_reset_callback(c6_reset_level, NULL) != ESP_TRANSPORT_OK) {
    SLog.println("[P4] FAIL: esp-hosted reset callback registration");
    return false;
  }

  // 3.3 V peripheral rail: LDO channel 4 (LilyGo's board init) — the SDMMC
  // IO supply (SOC_SDMMC_IO_POWER_EXTERNAL) and the touch controller's.
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id = 4;
  ldo.voltage_mv = 3300;
  if (esp_ldo_acquire_channel(&ldo, &s_periph_ldo) != ESP_OK) {
    SLog.println("[P4] FAIL: LDO channel 4 (3300 mV peripheral rail)");
    return false;
  }

  s_ready = true;
  SLog.printf("[P4] board up: XL9535 out=%02X/%02X, rails cycled (IO10 high, 5V high, 3V3 low), Ethernet PHY in reset, LDO4 3300 mV\n",
              s_out[0], s_out[1]);
  return true;
}

#endif // BOARD_TDISPLAY_P4
