// mic_dev.h — microphone capture: the per-board hardware layer and the
// board-neutral capture engine on top of it (mic_capture.cpp).
//
// One format everywhere: 16 kHz, mono, signed 16-bit, in a PSRAM ring of
// MIC_RING_SAMPLES samples. One owner at a time (mic_lua.cpp enforces it).
// A session mutes playback on every board: the engine calls sound_suspend()
// before the hardware starts and sound_resume() after it stops. On the Wio
// L2 and the T-Display P4 that is also what frees the shared I2S port's
// clock for the capture rate.
//
// Hardware layers: mic_tdeck.cpp (ES7210 on its own I2S port),
// mic_es8311.cpp (the codec's ADC on the speaker's port: T-Display P4),
// mic_wio_l2.cpp (ES7243E ADC on the speaker's port: Wio L2), mic_none.cpp
// (Heltec).

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef enum {
  MIC_DEV_NONE = 0,
  MIC_DEV_ES7210,     // T-Deck: ES7210 ADC, two analog mics
  MIC_DEV_ES8311,     // T-Display P4: the ES8311 codec's ADC
  MIC_DEV_ES7243E,    // Wio L2: ES7243E ADC beside the ES8311 DAC
} MicDevKind;

// ── Board hardware layer (one backend per build) ───────────────────────────

MicDevKind mic_hw_kind(void);

// Bring the ADC up at `rate_hz` and leave its I2S port receiving; `*port`
// receives the port number to read. Frames are stereo (two slots per frame);
// the engine keeps the slot mic_hw_mic_slot() names. False (logged) on
// failure.
bool mic_hw_start(uint32_t rate_hz, int* port);
void mic_hw_stop(void);

// Width of the slots the started port delivers: 16 (the slot is the 16-bit
// sample: ES8311) or 32 (the 16-bit sample is the top half of the slot word,
// the bottom half reads zero: ES7210).
uint8_t mic_hw_slot_bits(void);

// The slot of each frame that carries the microphone: 0 = left (the mic on
// the ES8311, mic 1 on the ES7210), 1 = right (the Wio L2's ES7243E).
uint8_t mic_hw_mic_slot(void);

// Gain step 0..7, 6 dB per step (ES8311: the REG16 ADC gain scale; ES7210
// and ES7243E: the PGA table, nearest entry). Returns the step applied, -1
// on failure.
int  mic_hw_set_gain_step(uint8_t step);

// ── Capture engine (mic_capture.cpp) ───────────────────────────────────────

#define MIC_RATE_HZ      16000
#define MIC_RING_SAMPLES 32768          // 2 s at 16 kHz, 64 KB of PSRAM

bool     mic_capture_start(void);       // suspends playback, starts the hardware + the reader task
void     mic_capture_stop(void);        // stops both, resumes playback
bool     mic_capture_reserve(bool on);  // on: the reader task now, parked between sessions;
                                        // off: it ends (after the session, if one runs).
                                        // False (logged) when its stack does not fit
bool     mic_capture_running(void);
size_t   mic_capture_read(int16_t* out, size_t max_samples);   // drains the ring
int16_t  mic_capture_peak(void);        // |sample| peak since the previous call
uint32_t mic_capture_dropped(void);     // samples lost to a full ring this session
