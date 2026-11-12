// mic_capture.cpp — see mic_dev.h: the board-neutral capture engine.
//
// A reader task (core 1, priority 2: below the sound task's 3 and the mesh
// task) blocks in i2s_read() on the port the hardware layer named and moves
// the left sample of every frame into the PSRAM ring (the top 16 bits of
// the left slot word where the port delivers 32-bit slots); the Lua side
// drains the ring from core 0. Single producer, single consumer: the
// unwrapped head and tail counters are the only shared state. A full ring
// drops the newest samples and counts them; the count is printed at stop.

#include "mic_dev.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "../meshpunk_sync.h"   // SLog
#include "../sound.h"           // sound_suspend / sound_resume

#define MIC_CHUNK_FRAMES 256    // stereo frames per i2s_read: 1 KB at 16-bit slots, 2 KB at 32-bit

static int16_t*          s_ring     = NULL;
static uint8_t           s_slot_bits = 16;   // mic_hw_slot_bits() of the running port
static volatile uint32_t s_head     = 0;    // next write (samples, unwrapped)
static volatile uint32_t s_tail     = 0;    // next read
static volatile bool     s_running  = false;
static volatile bool     s_stop_req = false;
static volatile int16_t  s_peak     = 0;
static volatile uint32_t s_dropped  = 0;
static int               s_port     = 0;
static TaskHandle_t      s_task     = NULL;

static void mic_reader_task(void* arg) {
  (void)arg;
  static int32_t chunk[MIC_CHUNK_FRAMES * 2];   // one read of stereo frames at 32-bit slots; 16-bit slots fill half
  const bool     wide        = (s_slot_bits == 32);
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
      int16_t s = wide ? (int16_t)(chunk[i * 2] >> 16)           // left slot, sample in its top 16 bits
                       : ((const int16_t*)chunk)[i * 2];         // left channel
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
  s_task = NULL;
  vTaskDelete(NULL);
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
  sound_suspend();                                 // playback muted for the session, every board
  if (!mic_hw_start(MIC_RATE_HZ, &s_port)) {
    sound_resume();
    return false;
  }
  s_slot_bits = mic_hw_slot_bits();
  s_running = true;
  if (xTaskCreatePinnedToCore(mic_reader_task, "mic_reader", 4096, NULL, 2, &s_task, 1) != pdPASS) {
    SLog.println("[MIC] FAIL: reader task");
    s_running = false;
    mic_hw_stop();
    sound_resume();
    return false;
  }
  SLog.printf("[MIC] capture on: %u Hz mono s16 from I2S%d (%u-bit slots), ring %u samples\n",
              (unsigned)MIC_RATE_HZ, s_port, (unsigned)s_slot_bits, (unsigned)MIC_RING_SAMPLES);
  return true;
}

void mic_capture_stop(void) {
  if (!s_running) return;
  s_stop_req = true;
  // The reader leaves within one i2s_read timeout; the port is torn down
  // only after it has gone.
  for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
  if (s_task) SLog.println("[MIC] reader task did not stop within 1 s");
  s_running = false;
  mic_hw_stop();
  sound_resume();
  SLog.printf("[MIC] capture off: %lu samples captured, %lu dropped\n",
              (unsigned long)s_head, (unsigned long)s_dropped);
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
