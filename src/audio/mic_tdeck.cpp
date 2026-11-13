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
// (mic_hw_inputs 2). All four inputs are powered, at one gain. The codec is
// configured once per boot, at 24 dB, and started and stopped per session.

#include "mic_dev.h"

#if defined(BOARD_TDECK)

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include "es7210.h"
#include "../tdeck-pins.h"          // TDECK_ES7210_*
#include "../meshpunk_sync.h"       // SLog

#define TDECK_MIC_I2S_PORT I2S_NUM_1

static bool s_port_up  = false;
static bool s_codec_up = false;

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

bool mic_hw_start(uint32_t rate_hz, int* port) {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = rate_hz;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;   // 32-bit slots, the sample in the top half (see above)
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;      // frames per buffer: 8 x 2 KB
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
    mic_hw_stop();
    return false;
  }

  if (!s_codec_up) {
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
      mic_hw_stop();
      return false;
    }
    es7210_adc_config_i2s(AUDIO_HAL_CODEC_MODE_ENCODE, &ccfg.i2s_iface);
    es7210_set_tdm(true);
    es7210_mic_select((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 |
                                            ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4));
    es7210_adc_set_gain_all(GAIN_24DB);
    s_codec_up = true;
  }
  es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
  *port = TDECK_MIC_I2S_PORT;
  SLog.printf("[MIC] ES7210 on I2S%d: MCLK %d, LRCK %d, SCK %d, data %d, %lu Hz\n",
              (int)TDECK_MIC_I2S_PORT, TDECK_ES7210_MCLK, TDECK_ES7210_LRCK,
              TDECK_ES7210_SCK, TDECK_ES7210_DIN, (unsigned long)rate_hz);
  return true;
}

void mic_hw_stop(void) {
  if (s_codec_up) es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
  if (s_port_up) {
    i2s_driver_uninstall(TDECK_MIC_I2S_PORT);
    s_port_up = false;
  }
}

int mic_hw_set_gain_step(uint8_t step) {
  if (!s_codec_up) return -1;
  if (step > 7) step = 7;
  return es7210_adc_set_gain_all(k_gain_steps[step]) == ESP_OK ? (int)step : -1;
}

#endif // BOARD_TDECK
