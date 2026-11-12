// es7243e.cpp — see es7243e.h.
//
// The register tables below are esp_codec_dev 1.6.2's es7243e.c write
// sequences in its order. TwoWire::endTransmission()'s code in the FAIL
// lines: 2 = ESP_FAIL from the I2C driver (any NACK, address or data),
// 4 = other error, 5 = timeout.

#include "es7243e.h"

#include <Arduino.h>
#include <Wire.h>

#include "../meshpunk_sync.h"   // SLog

static TwoWire* s_wire = nullptr;
static uint8_t  s_addr = 0;

struct Es7243eReg { uint8_t reg, val; };

// es7243e_open(), without the es7243e_adc_enable(true) it ends with.
static const Es7243eReg k_open[] = {
  {0x01, 0x3A}, {0x00, 0x80}, {0xF9, 0x00}, {0x04, 0x02}, {0x04, 0x01},
  {0xF9, 0x01}, {0x00, 0x1E}, {0x01, 0x00},
  {0x02, 0x00}, {0x03, 0x20}, {0x04, 0x01}, {0x0D, 0x00}, {0x05, 0x00},
  {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},                // SCLK = MCLK/4, LRCK = MCLK/256
  {0x09, 0xCA}, {0x0A, 0x85}, {0x0B, 0x00}, {0x0E, 0xBF}, {0x0F, 0x80},
  {0x14, 0x0C}, {0x15, 0x0C}, {0x17, 0x02}, {0x18, 0x26}, {0x19, 0x77},
  {0x1A, 0xF4}, {0x1B, 0x66}, {0x1C, 0x44}, {0x1E, 0x00}, {0x1F, 0x0C},
  {0x20, 0x1A}, {0x21, 0x1A},                              // PGA +30 dB
  {0x00, 0x80}, {0x01, 0x3A}, {0x16, 0x3F}, {0x16, 0x00},  // slave mode
};

// es7243e_adc_enable(true): slave mode.
static const Es7243eReg k_enable[] = {
  {0xF9, 0x00}, {0x04, 0x01}, {0x17, 0x01}, {0x20, 0x10}, {0x21, 0x10},
  {0x00, 0x80}, {0x01, 0x3A}, {0x16, 0x3F}, {0x16, 0x00},
};

// es7243e_adc_enable(false).
static const Es7243eReg k_disable[] = {
  {0x04, 0x02}, {0x04, 0x01}, {0xF7, 0x30}, {0xF9, 0x01}, {0x16, 0xFF},
  {0x17, 0x00}, {0x01, 0x38}, {0x20, 0x00}, {0x21, 0x00}, {0x00, 0x00},
  {0x00, 0x1E}, {0x01, 0x30}, {0x01, 0x00},
};

// One register write; TwoWire::endTransmission()'s code, 0 = ACKed.
static uint8_t wr(uint8_t reg, uint8_t val) {
  s_wire->beginTransmission(s_addr);
  s_wire->write(reg);
  s_wire->write(val);
  return s_wire->endTransmission();
}

static bool run(const Es7243eReg* seq, size_t n, const char* what) {
  for (size_t i = 0; i < n; i++) {
    uint8_t code = wr(seq[i].reg, seq[i].val);
    if (code != 0) {
      SLog.printf("[MIC] FAIL: ES7243E %s: write REG%02X=%02X (step %u), code %u\n",
                  what, seq[i].reg, seq[i].val, (unsigned)(i + 1), (unsigned)code);
      return false;
    }
  }
  return true;
}

bool es7243e_init(TwoWire& wire, uint8_t addr) {
  s_wire = &wire;
  s_addr = addr;
  s_wire->beginTransmission(s_addr);
  uint8_t code = s_wire->endTransmission();
  if (code != 0) {
    SLog.printf("[MIC] FAIL: ES7243E not answering at 0x%02X (code %u)\n", addr, (unsigned)code);
    s_wire = nullptr;
    return false;
  }
  if (!run(k_open, sizeof(k_open) / sizeof(k_open[0]), "open")) return false;
  if (!run(k_enable, sizeof(k_enable) / sizeof(k_enable[0]), "enable")) return false;
  SLog.printf("[MIC] ES7243E up at 0x%02X: I2S slave, MCLK from its pin\n", addr);
  return true;
}

bool es7243e_enable(bool on) {
  if (!s_wire) return false;
  return on ? run(k_enable, sizeof(k_enable) / sizeof(k_enable[0]), "enable")
            : run(k_disable, sizeof(k_disable) / sizeof(k_disable[0]), "disable");
}

bool es7243e_set_pga(uint8_t code) {
  if (!s_wire) return false;
  if (code > 14) code = 14;
  const Es7243eReg seq[] = { {0x20, (uint8_t)(0x10 | code)}, {0x21, (uint8_t)(0x10 | code)} };
  return run(seq, 2, "PGA");
}
