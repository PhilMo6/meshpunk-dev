// es7243e.h — Everest ES7243E two-channel audio ADC: I2C control for
// capture.
//
// Sets the ADC up as an I2S slave taking MCLK on its MCLK pin, with the
// register sequences of Espressif's esp_codec_dev ES7243E driver (esp-adf
// components/esp_codec_dev/device/es7243e/es7243e.c, Apache-2.0): its open,
// which ends with its enable sequence, and its enable / disable. That open
// sets the ADC's own clock dividers to SCLK = MCLK / 4 and LRCK = MCLK / 256,
// i.e. 32 SCLK per channel.
//
// Board-neutral: the board backend owns the I2C bus and the I2S pins.

#pragma once

#include <stdint.h>

class TwoWire;

// Address probe, then esp_codec_dev's open. False (logged: the address, or
// the failing register and TwoWire::endTransmission()'s code) on any I2C
// failure, including a chip that does not answer.
bool es7243e_init(TwoWire& wire, uint8_t addr);

// esp_codec_dev's enable / disable sequences: the ADC running, or its
// clocks and analog blocks powered down. The enable sequence leaves both
// PGAs at code 0 (0 dB). False (logged) on an I2C failure.
bool es7243e_enable(bool on);

// Both PGAs (REG20/21 = 0x10 | code), codes as esp_codec_dev's get_db_reg:
// code n = 3n dB for n = 0..11, then 12 = 34.5, 13 = 36, 14 = 37.5 dB.
// False (logged) on an I2C failure.
bool es7243e_set_pga(uint8_t code);
