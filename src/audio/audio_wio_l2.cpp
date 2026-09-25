// audio_wio_l2.cpp — Seeed Wio Tracker L2 audio backend
// (contract: audio_dev.h).
//
// The board's speaker path is an ES8311 codec (control on the shared I2C
// bus; I2S MCLK 10 / BCK 11 / WS 12 / DOUT 16 / DIN 15) feeding an amplifier
// whose enable is expander P14 (AUDIO_PA, parked OFF by the bring-up). The
// codec needs register configuration before I2S produces sound, and this
// backend does not configure it yet: the board reports no audio output, the
// sound.cpp mixer runs tone-less, and the amplifier stays off.
//
// The decode-only player works as on the Heltec kit, so music still plays
// through a USB audio sink.

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "audio_dev.h"
#include "Audio.h"
#include "../meshpunk_sync.h"   // SLog

Audio* audio_dev_init(void) {
  return nullptr;   // no configured output path yet
}

AudioDevKind audio_dev_kind(void) { return AUDIO_DEV_NONE; }

void audio_dev_buzzer_tone(uint32_t freq_hz) { (void)freq_hz; }
void audio_dev_buzzer_off(void) { }

// ── Decode-only player ─────────────────────────────────────────────────────
// Same instance as audio_heltec.cpp: no setPinout() (no GPIO claimed), its
// PCM is captured by sound.cpp's staging hook for the USB sink, and the DMA
// ring is 4x64 (1 KB of internal SRAM) with the stock size retried if the
// driver rejects it — see the Heltec backend for the full reasoning.
#define WIO_DEC_DMA_COUNT 4
#define WIO_DEC_DMA_LEN   64

Audio* audio_dev_decoder_open(void) {
  Audio* a = new Audio(false, 3, I2S_NUM_0,
                       WIO_DEC_DMA_COUNT, WIO_DEC_DMA_LEN);
  if (a && !a->i2sDriverInstalled()) {
    SLog.println("[AUDIO] decoder: minimal DMA rejected, retrying stock size");
    delete a;
    a = new Audio(false, 3, I2S_NUM_0);          // library defaults
    if (a && !a->i2sDriverInstalled()) { delete a; a = nullptr; }
  }
  if (!a) { SLog.println("[AUDIO] decoder open FAILED"); return nullptr; }
  SLog.printf("[AUDIO] decoder open (internal free %u)\n",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return a;
}

void audio_dev_decoder_close(Audio* a) {
  if (!a) return;
  delete a;   // destructor uninstalls the I2S driver and frees its buffers
  SLog.printf("[AUDIO] decoder closed (internal free %u)\n",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

#endif // BOARD_WIO_L2
