// audio_tdisplay_p4.cpp — LilyGo T-Display P4 audio backend (contract:
// audio_dev.h).
//
// ES8311 codec (I2C 0x18 on the board's second I2C bus, SDA 20 / SCL 21;
// src/audio/es8311.cpp) fed by I2S MCLK 13 / BCK 12 / WS 9 / DOUT 10
// (LilyGo's v1 config.h, `es8311` namespace; the codec's ADC data line 11 is
// not used), driving the speaker through an NS4150B amplifier whose power is
// expander IO6 (LilyGo `kAudioPowerEn`), high since the board layer's
// power sequence. The mixer, volume/mute prefs and the notification melody
// live above this in sound.cpp / notify.cpp, exactly as on the T-Deck's
// I2S path: the codec runs at 0 dB and the player's software volume sets
// loudness.
//
// The player is ESP32-audioI2S on IDF's legacy I2S driver (driver/i2s.h),
// which IDF 5.5.5 still builds for this chip and which the vendored library
// uses on every board.
//
// The codec's I2C bus is Arduino's Wire1 (I2C port 1): port 0 is the board
// layer's IDF-driver bus (tdisplay_p4_board.cpp) and cannot be opened a
// second time through Wire.

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <Wire.h>

#include "audio_dev.h"
#include "Audio.h"
#include "es8311.h"
#include "../meshpunk_sync.h"   // SLog

#define P4_I2S_MCLK       13
#define P4_I2S_BCK        12
#define P4_I2S_WS         9
#define P4_I2S_DOUT       10
#define P4_CODEC_SDA      20
#define P4_CODEC_SCL      21
#define P4_CODEC_I2C_HZ   400000
#define P4_ES8311_ADDR    0x18   // Espressif ES8311_ADDRESS_0 (CE low)

Audio* audio_dev_init(void) {
  if (!Wire1.begin(P4_CODEC_SDA, P4_CODEC_SCL, P4_CODEC_I2C_HZ)) {
    SLog.println("[AUDIO] FAIL: Wire1.begin on the codec bus (SDA 20 / SCL 21)");
  }
  Audio* audio = new Audio();
  audio->setPinout(P4_I2S_BCK, P4_I2S_WS, P4_I2S_DOUT, I2S_PIN_NO_CHANGE,
                   P4_I2S_MCLK);
  es8311_init(Wire1, P4_ES8311_ADDR);   // logs its own FAIL line
  return audio;
}

AudioDevKind audio_dev_kind(void) { return AUDIO_DEV_I2S; }

// Tone-translation drive: unused on the I2S path (sound.cpp mixes real PCM).
void audio_dev_buzzer_tone(uint32_t freq_hz) { (void)freq_hz; }
void audio_dev_buzzer_off(void) { }

// This board's player from audio_dev_init() already decodes; a second
// instance would fight it for the I2S port.
Audio* audio_dev_decoder_open(void) { return nullptr; }
void   audio_dev_decoder_close(Audio* a) { (void)a; }

#endif // BOARD_TDISPLAY_P4
