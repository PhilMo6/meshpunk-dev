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

// Set the ADC up ahead of the first session, once at boot (mic_capture_boot):
// the T-Deck's ES7210; a no-op on boards whose ADC is set up with the audio
// (ES8311) or by the first session (ES7243E). False (logged) on failure.
bool mic_hw_prepare(void);

// Keep the ADC running between sessions (on) or let it stop (off; a running
// session keeps it until mic_hw_stop). T-Deck: the I2S port and the ES7210
// stay up, so a session starts on settled hardware, with the frames the
// port queued before the start (mic_tdeck.cpp). T-Display P4 (the
// ES8311's ADC runs from es8311_init) and Wio L2 (the ES7243E is clocked by
// the speaker's port): nothing to hold, true. False (logged) on failure.
bool mic_hw_hold(bool on);

// Bring the ADC up at `rate_hz` and leave its I2S port receiving; `*port`
// receives the port number to read. Frames are stereo (two slots per frame);
// mic_hw_input_pos() names where each input's sample sits. False (logged)
// on failure.
bool mic_hw_start(uint32_t rate_hz, int* port);
void mic_hw_stop(void);

// Width of the slots the started port delivers: 16 (the slot is the 16-bit
// sample: ES8311) or 32 (32-bit slot words: ES7210, ES7243E).
uint8_t mic_hw_slot_bits(void);

// The microphones a session can choose between (mic_capture_set_input):
// 2 on the T-Deck (the ES7210's inputs 1 and 2), 1 on the other boards with
// a microphone, 0 without one. At most MIC_MAX_INPUTS.
#define MIC_MAX_INPUTS 4
uint8_t mic_hw_inputs(void);

// Where input n (1..mic_hw_inputs()) sits in each frame: *slot 0 = the left
// slot, 1 = the right; *shift = the bit position of its 16-bit sample in the
// slot word (16 = the top half of a 32-bit slot, 0 = the bottom half or a
// 16-bit slot).
void mic_hw_input_pos(uint8_t n, uint8_t* slot, uint8_t* shift);

// Gain step 0..7, 6 dB per step (ES8311: the REG16 ADC gain scale; ES7210
// and ES7243E: the PGA table, nearest entry). Returns the step applied, -1
// on failure.
int  mic_hw_set_gain_step(uint8_t step);

// ── Capture engine (mic_capture.cpp) ───────────────────────────────────────

#define MIC_RATE_HZ      16000
#define MIC_RING_SAMPLES 32768          // 2 s at 16 kHz, 64 KB of PSRAM

void     mic_capture_boot(void);        // mic_hw_prepare(), once in setup() after the I2C bus is up
bool     mic_capture_hold(bool on);     // mic_hw_hold() between sessions (mic_lua.cpp: while any app holds)
void     mic_capture_standby(bool asleep);   // standby: a hold's hardware goes down for the sleep
                                             // (true) and comes back after the wake (false)
bool     mic_capture_start(void);       // suspends playback, starts the hardware + the reader task
void     mic_capture_stop(void);        // stops both, resumes playback
bool     mic_capture_reserve(bool on);  // on: the reader task now, parked between sessions;
                                        // off: it ends (after the session, if one runs).
                                        // False (logged) when its stack does not fit
bool     mic_capture_running(void);
size_t   mic_capture_read(int16_t* out, size_t max_samples);   // drains the ring
int16_t  mic_capture_peak(void);        // |sample| peak since the previous call
uint32_t mic_capture_dropped(void);     // samples lost to a full ring this session
bool     mic_capture_set_input(uint8_t n);   // 1..mic_hw_inputs(), from the next session
                                             // on (until reboot); false out of range
uint8_t  mic_capture_input(void);       // the input sessions take (1 at boot)
