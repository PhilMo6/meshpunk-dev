// mic_wio_l2.cpp — mic_dev.h hardware layer for the Wio Tracker L2: the
// Everest ES7243E ADC (I2C 0x14 on Wire), its data out on GPIO 15, clocked
// by the speaker's I2S port (MCLK 10, BCK 11, WS 12) that also drives the
// ES8311 DAC. Source: Meshtastic's seeed_wio_tracker_L2 variant.h
// (HAS_ES7243E, ADC_I2S_DIN 15, ADC_I2S_MCLK/BCK/WS 10/11/12; the ES8311's
// own ADC output is not wired, DAC_I2S_DIN -1) and configuration.h
// (ES7243E_ADDR 0x14); Seeed's pin list names GPIO 15 CODEC_I2S0_SDOUT.
//
// A session reinstalls the player's port receive-only as the master
// (Audio::setCaptureInput) with 32-bit slots, the frame the ADC's own
// dividers describe (32 SCLK per channel, es7243e.h); the engine keeps the
// top 16 bits of the right slot word: with speech played at the board, the
// right slot carried it (session peak 1410) while the left read +-3 counts
// of noise, both at PGA +30 dB. The ADC is set up at the first session
// after boot (its MCLK comes from the port), enabled per session and powered
// down at the end; playback is suspended for the session.

#include "mic_dev.h"

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include "Audio.h"
#include "es7243e.h"
#include "../boards/board_pins.h"   // PIN_MIC_I2S_DIN
#include "../meshpunk_sync.h"       // SLog
#include "../sound.h"               // sound_audio

#define MIC_I2S_PORT      I2S_NUM_0   // the player's port (Audio's default)
#define WIO_ES7243E_ADDR  0x14        // Meshtastic configuration.h ES7243E_ADDR

static bool s_capture     = false;    // the port is in its capture install
static bool s_adc_up      = false;    // es7243e_init() done this boot
static bool s_adc_running = false;    // enabled for the current session

// 6 dB steps 0..42 onto the PGA codes (es7243e.h): 0, 6, 12, 18, 24, 30 dB,
// then 36 and 37.5 dB (the PGA's top two entries).
static const uint8_t k_pga_codes[8] = {0, 2, 4, 6, 8, 10, 13, 14};
static uint8_t s_pga_code = 10;       // 30 dB until the app sets a step

MicDevKind mic_hw_kind(void)      { return MIC_DEV_ES7243E; }
uint8_t    mic_hw_slot_bits(void) { return 32; }
uint8_t    mic_hw_mic_slot(void)  { return 1; }   // the right slot: see the header

bool mic_hw_start(uint32_t rate_hz, int* port) {
  Audio* a = sound_audio();
  if (!a) { SLog.println("[MIC] FAIL: no I2S player on this build"); return false; }
  if (!a->setCaptureInput(PIN_MIC_I2S_DIN, rate_hz, I2S_BITS_PER_SAMPLE_32BIT)) {
    SLog.println("[MIC] FAIL: I2S receive-only reinstall");
    return false;
  }
  s_capture = true;
  bool ok = s_adc_up ? es7243e_enable(true) : es7243e_init(Wire, WIO_ES7243E_ADDR);
  if (ok) {
    s_adc_up      = true;
    s_adc_running = true;
    ok = es7243e_set_pga(s_pga_code);
  }
  if (!ok) {
    mic_hw_stop();
    return false;
  }
  *port = MIC_I2S_PORT;
  SLog.printf("[MIC] ES7243E on I2S%d receive-only, 32-bit slots, data in GPIO %d, %lu Hz, PGA code %u\n",
              (int)MIC_I2S_PORT, PIN_MIC_I2S_DIN, (unsigned long)rate_hz, (unsigned)s_pga_code);
  return true;
}

void mic_hw_stop(void) {
  if (s_adc_running) {
    es7243e_enable(false);
    s_adc_running = false;
  }
  if (!s_capture) return;
  Audio* a = sound_audio();
  if (a && !a->clearDuplexInput()) SLog.println("[MIC] FAIL: I2S TX-only reinstall");
  s_capture = false;
}

int mic_hw_set_gain_step(uint8_t step) {
  if (step > 7) step = 7;
  s_pga_code = k_pga_codes[step];
  if (s_adc_running && !es7243e_set_pga(s_pga_code)) return -1;
  return (int)step;
}

#endif // BOARD_WIO_L2
