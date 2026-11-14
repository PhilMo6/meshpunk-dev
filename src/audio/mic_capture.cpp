// mic_capture.cpp — see mic_dev.h: the board-neutral capture engine.
//
// A reader task (core 1, priority 2: below the sound task's 3 and the mesh
// task) blocks in i2s_read() on the port the hardware layer named and moves
// the chosen input's sample of every frame (where mic_hw_input_pos() puts
// it) into the PSRAM ring; the Lua side drains the ring from core 0. Single
// producer, single consumer: the unwrapped head and tail counters are the
// only shared state. A full ring drops the newest samples and counts them;
// the count is printed at stop, with every input's RMS and peak.
//
// The reader task normally lives for one session. mic_capture_reserve(true)
// creates it ahead of the sessions and keeps it parked between them, so a
// session start needs no new memory (its stack must be internal RAM, the
// scarcest pool); mic_capture_reserve(false) lets it end.

#include "mic_dev.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "../meshpunk_sync.h"   // SLog
#include "../sound.h"           // sound_suspend / sound_resume

#define MIC_CHUNK_FRAMES 256    // stereo frames per i2s_read: 1 KB at 16-bit slots, 2 KB at 32-bit

static int16_t*          s_ring     = NULL;
static uint8_t           s_slot_bits = 16;   // mic_hw_slot_bits() of the running port
static uint8_t           s_input     = 1;    // mic_capture_set_input(): the input sessions take
static uint8_t           s_n_inputs  = 0;    // mic_hw_inputs() of the running session
static uint8_t           s_pos_slot[MIC_MAX_INPUTS];    // mic_hw_input_pos() of each input
static uint8_t           s_pos_shift[MIC_MAX_INPUTS];
static volatile uint32_t s_head     = 0;    // next write (samples, unwrapped)
static volatile uint32_t s_tail     = 0;    // next read
static volatile bool     s_running  = false;
static volatile bool     s_stop_req = false;
static volatile int16_t  s_peak     = 0;
static volatile uint32_t s_dropped  = 0;
static int               s_port     = 0;

// Every input's level over the whole session, printed at stop.
static uint32_t          s_sess_n = 0;                   // frames counted
static uint64_t          s_sess_sq[MIC_MAX_INPUTS];      // sums of squares
static int32_t           s_sess_peak[MIC_MAX_INPUTS];    // |sample| peaks
static TaskHandle_t      s_task     = NULL;
static volatile bool     s_go       = false;  // a session asks the reader to read; the reader clears it when it stops
static volatile bool     s_exit_req = false;  // the parked reader ends
static bool              s_reserved = false;  // mic_capture_reserve(true): the reader stays between sessions

#define MIC_READER_STACK 4096

// One session's reads, until s_stop_req.
static void read_session(void) {
  static int32_t chunk[MIC_CHUNK_FRAMES * 2];   // one read of stereo frames at 32-bit slots; 16-bit slots fill half
  const bool     wide        = (s_slot_bits == 32);
  const size_t   frame_bytes = wide ? 8 : 4;
  const uint8_t  ni          = s_n_inputs;
  const uint8_t  sel         = (uint8_t)(s_input - 1);
  while (!s_stop_req) {
    size_t got = 0;
    esp_err_t err = i2s_read((i2s_port_t)s_port, chunk, MIC_CHUNK_FRAMES * frame_bytes, &got, pdMS_TO_TICKS(200));
    if (err != ESP_OK || got == 0) continue;
    size_t   nframes = got / frame_bytes;
    uint32_t head    = s_head;
    uint32_t tail    = s_tail;
    int32_t  peak    = s_peak;
    for (size_t i = 0; i < nframes; i++) {
      int16_t v[MIC_MAX_INPUTS];
      for (uint8_t k = 0; k < ni; k++) {
        v[k] = wide ? (int16_t)(chunk[i * 2 + s_pos_slot[k]] >> s_pos_shift[k])
                    : ((const int16_t*)chunk)[i * 2 + s_pos_slot[k]];
      }
      for (uint8_t k = 0; k < ni; k++) {
        int32_t ak = v[k] < 0 ? -(int32_t)v[k] : v[k];
        if (ak > s_sess_peak[k]) s_sess_peak[k] = ak;
        s_sess_sq[k] += (uint32_t)(ak * ak);         // <= 2^30
      }
      s_sess_n++;
      int16_t s = v[sel];
      int32_t a = s < 0 ? -(int32_t)s : s;
      if (a > peak) peak = a;
      if (head - tail >= MIC_RING_SAMPLES) { s_dropped = s_dropped + 1; continue; }
      s_ring[head % MIC_RING_SAMPLES] = s;
      head++;
    }
    if (peak > 32767) peak = 32767;
    s_peak = (int16_t)peak;
    s_head = head;
  }
}

// Parked until a session sets s_go (and notifies), reads it, parks again;
// ends when asked while parked, or after a session when not reserved.
static void mic_reader_task(void* arg) {
  (void)arg;
  for (;;) {
    while (!s_go) {
      if (s_exit_req) { s_task = NULL; vTaskDelete(NULL); }
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    read_session();
    s_go = false;
    if (!s_reserved) break;
  }
  s_task = NULL;
  vTaskDelete(NULL);
}

static bool reader_create(void) {
  s_go = false;
  s_exit_req = false;
  if (xTaskCreatePinnedToCore(mic_reader_task, "mic_reader", MIC_READER_STACK, NULL, 2, &s_task, 1) != pdPASS) {
    s_task = NULL;
    SLog.printf("[MIC] FAIL: reader task (%u B stack; internal RAM largest free %u B)\n",
                (unsigned)MIC_READER_STACK,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return false;
  }
  return true;
}

// The parked reader ends; returns once it has.
static void reader_end(void) {
  if (!s_task) return;
  s_exit_req = true;
  xTaskNotifyGive(s_task);
  for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
  if (s_task) SLog.println("[MIC] reader task did not end within 1 s");
}

bool mic_capture_reserve(bool on) {
  if (on) {
    if (!s_task && !reader_create()) return false;
    s_reserved = true;
    return true;
  }
  s_reserved = false;
  if (!s_running) reader_end();
  return true;
}

void mic_capture_boot(void) {
  if (mic_hw_kind() == MIC_DEV_NONE) return;
  if (!mic_hw_prepare()) SLog.println("[MIC] FAIL: microphone set-up at boot");
}

// s_held: an app holds the hardware; s_asleep: standby has it down.
static bool s_held   = false;
static bool s_asleep = false;

bool mic_capture_hold(bool on) {
  if (mic_hw_kind() == MIC_DEV_NONE) return !on;
  if (on == s_held) return true;
  if (on && !s_asleep && !mic_hw_hold(true)) return false;
  s_held = on;
  if (!on && !s_asleep) mic_hw_hold(false);
  SLog.printf("[MIC] hardware %s between sessions\n", on ? "held" : "released");
  return true;
}

void mic_capture_standby(bool asleep) {
  if (asleep == s_asleep) return;
  s_asleep = asleep;
  if (!s_held) return;
  if (asleep) {
    mic_hw_hold(false);
  } else if (!mic_hw_hold(true)) {
    SLog.println("[MIC] FAIL: the held hardware did not come back after standby");
  }
}

bool mic_capture_start(void) {
  if (s_running) return true;
  if (mic_hw_kind() == MIC_DEV_NONE) {
    SLog.println("[MIC] no microphone on this board");
    return false;
  }
  if (!s_ring) {
    s_ring = (int16_t*)heap_caps_malloc(MIC_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) { SLog.println("[MIC] FAIL: ring allocation (PSRAM)"); return false; }
  }
  s_head = 0; s_tail = 0; s_peak = 0; s_dropped = 0; s_stop_req = false;
  s_sess_n = 0;
  for (uint8_t k = 0; k < MIC_MAX_INPUTS; k++) { s_sess_sq[k] = 0; s_sess_peak[k] = 0; }
  s_n_inputs = mic_hw_inputs();
  if (s_n_inputs < 1 || s_n_inputs > MIC_MAX_INPUTS || s_input > s_n_inputs) {
    SLog.printf("[MIC] FAIL: input %u of %u (the engine takes 1..%u inputs)\n",
                (unsigned)s_input, (unsigned)s_n_inputs, (unsigned)MIC_MAX_INPUTS);
    return false;
  }
  for (uint8_t k = 0; k < s_n_inputs; k++) mic_hw_input_pos((uint8_t)(k + 1), &s_pos_slot[k], &s_pos_shift[k]);
  sound_suspend();                                 // playback muted for the session, every board
  if (!mic_hw_start(MIC_RATE_HZ, &s_port)) {
    sound_resume();
    return false;
  }
  s_slot_bits = mic_hw_slot_bits();
  if (!s_task && !reader_create()) {
    mic_hw_stop();
    sound_resume();
    return false;
  }
  s_running = true;
  s_go = true;
  xTaskNotifyGive(s_task);
  SLog.printf("[MIC] capture on: %u Hz mono s16 from I2S%d (%u-bit slots), input %u of %u (%s slot, bit %u), ring %u samples%s\n",
              (unsigned)MIC_RATE_HZ, s_port, (unsigned)s_slot_bits, (unsigned)s_input, (unsigned)s_n_inputs,
              s_pos_slot[s_input - 1] ? "right" : "left", (unsigned)s_pos_shift[s_input - 1],
              (unsigned)MIC_RING_SAMPLES, s_reserved ? ", reserved reader" : "");
  return true;
}

void mic_capture_stop(void) {
  if (!s_running) return;
  s_stop_req = true;
  // The reader leaves its session within one i2s_read timeout (then parks,
  // or ends when not reserved); the port is torn down only after that.
  for (int i = 0; i < 100 && s_go; i++) vTaskDelay(pdMS_TO_TICKS(10));
  if (s_go) SLog.println("[MIC] reader task did not stop within 1 s");
  if (!s_reserved) {
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task) SLog.println("[MIC] reader task did not end within 1 s");
  }
  s_running = false;
  mic_hw_stop();
  sound_resume();
  uint32_t n = s_sess_n;
  char     lv[200];
  size_t   len = 0;
  lv[0] = 0;
  for (uint8_t k = 0; k < s_n_inputs && len < sizeof(lv); k++) {
    long rms = n ? (long)(sqrt((double)s_sess_sq[k] / n) + 0.5) : 0;
    len += (size_t)snprintf(lv + len, sizeof(lv) - len, "%sinput %u RMS %ld peak %ld",
                            k ? ", " : "", (unsigned)(k + 1), rms, (long)s_sess_peak[k]);
  }
  SLog.printf("[MIC] capture off: %lu samples captured, %lu dropped; over %lu frames: %s\n",
              (unsigned long)s_head, (unsigned long)s_dropped, (unsigned long)n, lv);
}

bool mic_capture_running(void) { return s_running; }

size_t mic_capture_read(int16_t* out, size_t max_samples) {
  uint32_t head  = s_head;
  uint32_t tail  = s_tail;
  size_t   avail = (size_t)(head - tail);
  size_t   n     = avail < max_samples ? avail : max_samples;
  for (size_t i = 0; i < n; i++) out[i] = s_ring[(tail + i) % MIC_RING_SAMPLES];
  s_tail = tail + (uint32_t)n;
  return n;
}

int16_t mic_capture_peak(void) {
  int16_t p = s_peak;
  s_peak = 0;
  return p;
}

uint32_t mic_capture_dropped(void) { return s_dropped; }

bool mic_capture_set_input(uint8_t n) {
  if (n < 1 || n > mic_hw_inputs()) return false;
  s_input = n;
  return true;
}

uint8_t mic_capture_input(void) { return s_input; }
