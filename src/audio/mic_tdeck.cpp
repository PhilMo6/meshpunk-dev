// mic_tdeck.cpp — mic_dev.h hardware layer for the T-Deck: the ES7210 ADC
// (four analog inputs) on its own I2S port.
//
// The speaker holds I2S_NUM_0 (the player's default port); the ES7210 takes
// I2S_NUM_1 as a slave with the port's MCLK (256 x fs) on GPIO 48, LRCK 21,
// SCK 47, data 14 (tdeck-pins.h), over the shared I2C bus (Wire, address
// 0x40: AD1/AD0 low). Its registers are set by lib/es7210 (Espressif's
// driver): I2S format, 16-bit word length, TDM (es7210_set_tdm). The
// schematic has a mic footprint on each of the four inputs (MIC1-MIC4, bias
// MICBIAS12 and MICBIAS34) and one data line to GPIO 14, joined through R7
// to the codec's pin 11 (SDOUT1/TDMOUT) and through R8 to its pin 12
// (SDOUT2/TDMIN). Outside TDM both pins drive the line and the samples read
// back as the bitwise AND of inputs 1-2 and inputs 3-4 (each low bit set in
// 25 % of the samples, not 50 %); in TDM pin 12 is an input and pin 11 sends
// the four inputs back to back, 16 bits each, filling the port's frame of
// two 32-bit slots (64 SCLK, the frame LilyGo's T-Deck UnitTest clocks it
// with): inputs 1 and 2 are the top and bottom halves of the left slot word,
// 3 and 4 of the right (mic_hw_input_pos). The board's two microphones are
// inputs 1 and 2 (speech at RMS 270-300 on both at 24 dB, inputs 3 and 4 at
// RMS 40-45 in the same takes), so a session chooses between those two
// (mic_hw_inputs 2). All four inputs are powered, at one gain.
//
// The codec is set up once, at boot (mic_hw_prepare, with the port up), at
// 24 dB. Starting it (es7210_start) powers the mic bias, PGAs and ADCs up,
// and the output steps and decays until about 125 ms after the start: in
// recordings that began about 60 ms after a start, input 1 began at -2191
// and decayed to the noise floor about 110 ms after the start, and input 2
// stepped to full scale about 65 ms after it and was at the noise floor by
// about 125 ms. While an app holds the microphone (mic_hw_hold) the port and
// the codec keep running between sessions. A session on a codec that has
// run for TDECK_MIC_SETTLE_MS plus rx_span_ms() keeps the frames the port
// has queued, so its first frames are the ones captured just before it
// started. Any other session waits until TDECK_MIC_SETTLE_MS after the
// codec's start and drops every queued frame; unheld, it starts the codec
// itself and the stop powers it down.

#include "mic_dev.h"

#if defined(BOARD_TDECK)

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include "es7210.h"
#include "../tdeck-pins.h"          // TDECK_ES7210_*
#include "../meshpunk_sync.h"       // SLog

#define TDECK_MIC_I2S_PORT   I2S_NUM_1
#define TDECK_MIC_SETTLE_MS  150        // from a codec start to the first frame (see the header)
#define TDECK_MIC_DMA_BUFS   8          // the port's RX DMA buffers
#define TDECK_MIC_DMA_FRAMES 256        // frames per buffer: 2 KB, one reader chunk (MIC_CHUNK_FRAMES)

static bool     s_port_up     = false;
static bool     s_codec_up    = false;  // registers set (codec_setup)
static bool     s_codec_on    = false;  // started (es7210_start): powered and sending
static uint32_t s_codec_on_ms = 0;      // millis() at that start
static bool     s_hold        = false;  // mic_hw_hold(true)
static bool     s_session     = false;  // between mic_hw_start and mic_hw_stop

// 6 dB steps 0..42 onto the chip's 3 dB table (its top entry is 37.5 dB).
static const es7210_gain_value_t k_gain_steps[8] = {
  GAIN_0DB, GAIN_6DB, GAIN_12DB, GAIN_18DB, GAIN_24DB, GAIN_30DB, GAIN_36DB, GAIN_37_5DB,
};

MicDevKind mic_hw_kind(void)   { return MIC_DEV_ES7210; }
uint8_t    mic_hw_slot_bits(void) { return 32; }
uint8_t    mic_hw_inputs(void)    { return 2; }

void mic_hw_input_pos(uint8_t n, uint8_t* slot, uint8_t* shift) {
  *slot  = (uint8_t)((n - 1) / 2);
  *shift = ((n - 1) % 2) ? 0 : 16;
}

static void port_down(void) {
  if (!s_port_up) return;
  i2s_driver_uninstall(TDECK_MIC_I2S_PORT);
  s_port_up = false;
}

// The port as the master, receive-only, MCLK on.
static bool port_up(uint32_t rate_hz) {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = rate_hz;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;   // 32-bit slots, the sample in the top half (see above)
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = TDECK_MIC_DMA_BUFS;
  cfg.dma_buf_len          = TDECK_MIC_DMA_FRAMES;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = false;
  cfg.fixed_mclk           = 0;
  cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan        = I2S_BITS_PER_CHAN_DEFAULT;  // = the 32-bit sample
  esp_err_t err = i2s_driver_install(TDECK_MIC_I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    SLog.printf("[MIC] FAIL: I2S%d install: %s\n", (int)TDECK_MIC_I2S_PORT, esp_err_to_name(err));
    return false;
  }
  s_port_up = true;
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = TDECK_ES7210_MCLK;
  pins.bck_io_num   = TDECK_ES7210_SCK;
  pins.ws_io_num    = TDECK_ES7210_LRCK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = TDECK_ES7210_DIN;
  err = i2s_set_pin(TDECK_MIC_I2S_PORT, &pins);
  if (err != ESP_OK) {
    SLog.printf("[MIC] FAIL: I2S%d pins: %s\n", (int)TDECK_MIC_I2S_PORT, esp_err_to_name(err));
    port_down();
    return false;
  }
  return true;
}

// The codec's registers; runs with the port up. Leaves the codec set up as
// es7210_adc_init does, not started.
static bool codec_setup(void) {
  audio_hal_codec_config_t ccfg = {};
  ccfg.adc_input         = AUDIO_HAL_ADC_INPUT_ALL;
  ccfg.dac_output        = AUDIO_HAL_DAC_OUTPUT_ALL;
  ccfg.codec_mode        = AUDIO_HAL_CODEC_MODE_ENCODE;
  ccfg.i2s_iface.mode    = AUDIO_HAL_MODE_SLAVE;
  ccfg.i2s_iface.fmt     = AUDIO_HAL_I2S_NORMAL;
  ccfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  ccfg.i2s_iface.bits    = AUDIO_HAL_BIT_LENGTH_16BITS;
  if (es7210_adc_init(&Wire, &ccfg) != ESP_OK) {
    SLog.println("[MIC] FAIL: ES7210 init (I2C 0x40)");
    return false;
  }
  es7210_adc_config_i2s(AUDIO_HAL_CODEC_MODE_ENCODE, &ccfg.i2s_iface);
  es7210_set_tdm(true);
  es7210_mic_select((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 |
                                          ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4));
  es7210_adc_set_gain_all(GAIN_24DB);
  s_codec_up = true;
  return true;
}

// Drops every frame the port has queued.
static void drain_rx(void) {
  uint8_t scratch[512];
  size_t  got;
  do {
    got = 0;
    i2s_read(TDECK_MIC_I2S_PORT, scratch, sizeof(scratch), &got, 0);
  } while (got > 0);
}

// The oldest frame the port can hand over is at most this old: the queued
// buffers (at most TDECK_MIC_DMA_BUFS) plus the one the DMA is filling.
static uint32_t rx_span_ms(uint32_t rate_hz) {
  return (uint32_t)((TDECK_MIC_DMA_BUFS + 1) * TDECK_MIC_DMA_FRAMES * 1000u / rate_hz);
}

bool mic_hw_prepare(void) {
  if (s_codec_up) return true;
  if (!port_up(MIC_RATE_HZ)) return false;
  bool ok = codec_setup();
  if (ok) es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
  port_down();
  if (ok) SLog.println("[MIC] ES7210 set up: TDM, inputs 1-4 at 24 dB; stopped until a session");
  return ok;
}

// The port up and the codec started; a no-op while both already run.
static bool run_up(uint32_t rate_hz) {
  if (s_codec_on) return true;
  if (!s_port_up && !port_up(rate_hz)) return false;
  if (!s_codec_up && !codec_setup()) {
    port_down();
    return false;
  }
  es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
  s_codec_on    = true;
  s_codec_on_ms = millis();
  return true;
}

// The codec powered down and the port removed.
static void run_down(void) {
  if (s_codec_on) {
    es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
    s_codec_on = false;
  }
  port_down();
}

bool mic_hw_hold(bool on) {
  if (on) {
    if (!run_up(MIC_RATE_HZ)) return false;
    s_hold = true;
    return true;
  }
  s_hold = false;
  if (!s_session) run_down();
  return true;
}

bool mic_hw_start(uint32_t rate_hz, int* port) {
  if (!run_up(rate_hz)) return false;
  uint32_t since = millis() - s_codec_on_ms;
  bool     keep  = since >= TDECK_MIC_SETTLE_MS + rx_span_ms(rate_hz);
  uint32_t wait  = 0;
  if (!keep) {
    if (since < TDECK_MIC_SETTLE_MS) wait = TDECK_MIC_SETTLE_MS - since;
    if (wait) vTaskDelay(pdMS_TO_TICKS(wait));
    drain_rx();
  }
  s_session = true;
  *port = TDECK_MIC_I2S_PORT;
  char how[48];
  if (keep) snprintf(how, sizeof(how), "the queued frames kept");
  else      snprintf(how, sizeof(how), "waited %lu ms, the queued frames dropped", (unsigned long)wait);
  SLog.printf("[MIC] ES7210 on I2S%d: MCLK %d, LRCK %d, SCK %d, data %d, %lu Hz; running for %lu ms, %s%s\n",
              (int)TDECK_MIC_I2S_PORT, TDECK_ES7210_MCLK, TDECK_ES7210_LRCK,
              TDECK_ES7210_SCK, TDECK_ES7210_DIN, (unsigned long)rate_hz,
              (unsigned long)since, how, s_hold ? " (held)" : "");
  return true;
}

void mic_hw_stop(void) {
  s_session = false;
  if (!s_hold) run_down();
}

int mic_hw_set_gain_step(uint8_t step) {
  if (!s_codec_up) return -1;
  if (step > 7) step = 7;
  return es7210_adc_set_gain_all(k_gain_steps[step]) == ESP_OK ? (int)step : -1;
}

#endif // BOARD_TDECK
