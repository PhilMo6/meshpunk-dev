// es8311.h — Everest ES8311 audio codec: I2C control for DAC playback.
//
// Configures the codec as an I2S slave taking MCLK on its MCLK pin at
// 256 x fs (the ESP32 I2S master default, mclk_multiple 0), 16-bit I2S
// format in and out. At 256 x fs every clock-divider coefficient is the
// same for all sample rates 8-64 kHz, so one init covers every rate the
// player switches to. Register sequence = Espressif's esp_codec_dev ES8311
// driver (esp-adf components/esp_codec_dev/device/es8311, Apache-2.0).
//
// Board-neutral: the board backend owns the I2C bus, the I2S pins and the
// speaker amplifier.

#pragma once

#include <stdint.h>

class TwoWire;

// Full codec setup (esp_codec_dev open, sample config, format, start), DAC
// digital volume 0 dB (the player's software volume sets loudness),
// unmuted. False (logged) on any I2C failure.
bool es8311_init(TwoWire& wire, uint8_t addr);

// ADC gain scale, REG16 bits 2:0: 6 dB per step, 0..7 = 0..42 dB (the
// steps of Espressif's es8311_mic_gain_t). es8311_init() leaves step 4
// (REG16 = 0x24) with the analog PGA at REG14's init value; this changes
// the scale only. The ADC itself runs from init on: Espressif's start
// sequence powers it (REG0E) and unmutes its serial output (REG0A bit 6).
// False (logged) on an I2C failure.
bool es8311_mic_gain_set(uint8_t step);
