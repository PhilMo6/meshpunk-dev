// mic_capture.cpp — see mic_dev.h: the board-neutral capture engine.
//
// A reader task (core 1, priority 2: below the sound task's 3 and the mesh
// task) blocks in i2s_read() on the port the hardware layer named and moves
// the microphone's sample of every frame into the PSRAM ring (the slot
// mic_hw_mic_slot() names; the top 16 bits of the slot word where the port
// delivers 32-bit slots); the Lua side
// drains the ring from core 0. Single producer, single consumer: the
// unwrapped head and tail counters are the only shared state. A full ring
// drops the newest samples and counts them; the count is printed at stop,
// with the session's left and right slot peaks.
//
// The reader task normally lives for one session. mic_capture_reserve(true)
// creates it ahead of the sessions and keeps it parked between them, so a
// session start needs no new memory (its stack must be internal RAM, the
// scarcest pool); mic_capture_reserve(false) lets it end.

#include "mic_dev.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "../meshpunk_sync.h"   // SLog
#include "../sound.h"           // sound_suspend / sound_resume

#define MIC_CHUNK_FRAMES 256    // stereo frames per i2s_read: 1 KB at 16-bit slots, 2 KB at 32-bit

static int16_t*          s_ring     = NULL;
static uint8_t           s_slot_bits = 16;   // mic_hw_slot_bits() of the running port
static uint8_t           s_mic_slot  = 0;    // mic_hw_mic_slot(): 0 = left, 1 = right
static volatile uint32_t s_head     = 0;    // next write (samples, unwrapped)
static volatile uint32_t s_tail     = 0;    // next read
static volatile bool     s_running  = false;
static volatile bool     s_stop_req = false;
static volatile int16_t  s_peak     = 0;
static volatile uint32_t s_dropped  = 0;
static int32_t           s_sess_peak_l = 0;  // |sample| peaks of both slots over the session,
static int32_t           s_sess_peak_r = 0;  // printed at stop
static int               s_port     = 0;
static TaskHandle_t      s_task     = NULL;
static volatile bool     s_go       = false;  // a session asks the reader to read; the reader clears it when it stops
static volatile bool     s_exit_req = false;  // the parked reader ends
static bool              s_reserved = false;  // mic_capture_reserve(true): the reader stays between sessions

#define MIC_READER_STACK 4096

// One session's reads, until s_stop_req.
static void read_session(void) {
  static int32_t chunk[MIC_CHUNK_FRAMES * 2];   // one read of stereo frames at 32-bit slots; 16-bit slots fill half
  const bool     wide        = (s_slot_bits == 32);
  const bool     right       = (s_mic_slot == 1);
  const size_t   frame_bytes = wide ? 8 : 4;
  while (!s_stop_req) {
    size_t got = 0;
    esp_err_t err = i2s_read((i2s_port_t)s_port, chunk, MIC_CHUNK_FRAMES * frame_bytes, &got, pdMS_TO_TICKS(200));
    if (err != ESP_OK || got == 0) continue;
    size_t   nframes = got / frame_bytes;
    uint32_t head    = s_head;
    uint32_t tail    = s_tail;
    int32_t  peak    = s_peak;
    for (size_t i = 0; i < nframes; i++) {
      int16_t l = wide ? (int16_t)(chunk[i * 2] >> 16)           // 32-bit slot: sample in its top 16 bits
                       : ((const int16_t*)chunk)[i * 2];
      int16_t r = wide ? (int16_t)(chunk[i * 2 + 1] >> 16)
                       : ((const int16_t*)chunk)[i * 2 + 1];
      int32_t al = l < 0 ? -(int32_t)l : l;
      int32_t ar = r < 0 ? -(int32_t)r : r;
      if (al > s_sess_peak_l) s_sess_peak_l = al;
      if (ar > s_sess_peak_r) s_sess_peak_r = ar;
      int16_t s = right ? r : l;
      int32_t a = right ? ar : al;
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
  s_sess_peak_l = 0; s_sess_peak_r = 0;
  sound_suspend();                                 // playback muted for the session, every board
  if (!mic_hw_start(MIC_RATE_HZ, &s_port)) {
    sound_resume();
    return false;
  }
  s_slot_bits = mic_hw_slot_bits();
  s_mic_slot  = mic_hw_mic_slot();
  if (!s_task && !reader_create()) {
    mic_hw_stop();
    sound_resume();
    return false;
  }
  s_running = true;
  s_go = true;
  xTaskNotifyGive(s_task);
  SLog.printf("[MIC] capture on: %u Hz mono s16 from I2S%d (%u-bit slots, %s slot), ring %u samples%s\n",
              (unsigned)MIC_RATE_HZ, s_port, (unsigned)s_slot_bits, s_mic_slot ? "right" : "left",
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
  SLog.printf("[MIC] capture off: %lu samples captured, %lu dropped; session peak left %ld, right %ld\n",
              (unsigned long)s_head, (unsigned long)s_dropped, (long)s_sess_peak_l, (long)s_sess_peak_r);
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
