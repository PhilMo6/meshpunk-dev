// mic_es8311.cpp — mic_dev.h hardware layer for the T-Display P4, whose
// ES8311 codec is both speaker and microphone (the codec's data out on
// GPIO 11, PIN_MIC_I2S_DIN in boards/board_pins.h). The Wio L2's microphone
// is a separate ADC: mic_wio_l2.cpp.
//
// The codec's ADC runs from es8311_init() on (Espressif's start sequence
// powers it and unmutes its serial output), so a session only has to read
// the line: the player's I2S port is reinstalled with RX and the data-in
// pin (Audio::setDuplexInput, a MESHPUNK addition to the vendored player)
// and set to the capture rate. The player's own rate returns with its next
// track; sound_resume() re-arms the tone rate. One I2S clock per port is
// why playback is suspended for the session.

#include "mic_dev.h"

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <driver/i2s.h>
#include "Audio.h"
#include "es8311.h"
#include "../boards/board_pins.h"   // PIN_MIC_I2S_DIN
#include "../meshpunk_sync.h"       // SLog
#include "../sound.h"               // sound_audio

#define MIC_I2S_PORT I2S_NUM_0      // the player's port (Audio's default)

static bool s_duplex = false;

MicDevKind mic_hw_kind(void)      { return MIC_DEV_ES8311; }
uint8_t    mic_hw_slot_bits(void) { return 16; }
uint8_t    mic_hw_mic_slot(void)  { return 0; }

bool mic_hw_start(uint32_t rate_hz, int* port) {
  Audio* a = sound_audio();
  if (!a) { SLog.println("[MIC] FAIL: no I2S player on this build"); return false; }
  if (!a->setDuplexInput(PIN_MIC_I2S_DIN)) {
    SLog.println("[MIC] FAIL: I2S duplex reinstall");
    return false;
  }
  s_duplex = true;
  esp_err_t err = i2s_set_sample_rates(MIC_I2S_PORT, rate_hz);
  if (err != ESP_OK) {
    SLog.printf("[MIC] FAIL: I2S rate %lu Hz: %s\n", (unsigned long)rate_hz, esp_err_to_name(err));
    a->clearDuplexInput();
    s_duplex = false;
    return false;
  }
  *port = MIC_I2S_PORT;
  SLog.printf("[MIC] ES8311 ADC on I2S%d duplex, data in GPIO %d, %lu Hz\n",
              (int)MIC_I2S_PORT, PIN_MIC_I2S_DIN, (unsigned long)rate_hz);
  return true;
}

void mic_hw_stop(void) {
  if (!s_duplex) return;
  Audio* a = sound_audio();
  if (a && !a->clearDuplexInput()) SLog.println("[MIC] FAIL: I2S TX-only reinstall");
  s_duplex = false;
}

int mic_hw_set_gain_step(uint8_t step) {
  if (step > 7) step = 7;
  return es8311_mic_gain_set(step) ? (int)step : -1;
}

#endif // BOARD_TDISPLAY_P4
