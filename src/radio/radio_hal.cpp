#include "radio_hal.h"
#include "radio_chip.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <ctype.h>
#include <string.h>

#include "../meshpunk_sync.h"   // SPI_LOCK/UNLOCK, SLog

#if defined(BOARD_HELTEC_V4)
#include "../boards/punk_heltec_board.h"   // heltec_radiated_to_chip_dbm()
#endif
#if defined(BOARD_TDISPLAY_P4)
#include <SPI.h>
#include "../boards/board_pins.h"          // PIN_LORA_* / PIN_SPI_* for the SKU probe
#include "../boards/tdisplay_p4_board.h"   // expander: radio reset, antenna switch
#endif

// The chip behind the HAL (radio_chip.h): nullptr until radio_hal_init(),
// and again after a failed begin() — every raw op then returns its
// no-radio result at once.
static RadioChip* s_chip = nullptr;
// main.cpp's SX1262 object (RADIO_CLASS radio): the chip on the SX1262
// boards and on the T-Display P4's SX1262 SKU.
static SX1262*    s_sx1262 = nullptr;

// Last-programmed state the recovery/score paths need back: operating
// frequency (image recalibration after an AGC reset), spreading factor
// (per-SF packet scoring), rx boost (re-applied after an AGC reset).
static float   s_last_freq_mhz = 0.0f;
static uint8_t s_last_sf = 10;
static bool    s_rx_boost = false;

// Antenna selection on boards with a switch (radio_hal.h): false = internal.
static bool    s_antenna_external = false;
// A transmission is on the air: start_send accepted, send_finished (or a
// standby/sleep) not yet called. The antenna switch waits for it to clear.
static volatile bool s_tx_active = false;
// The transmission in flight, for the send_finished log line.
static int      s_tx_len = 0;
static uint32_t s_tx_start_ms = 0;

// Radiated-dBm (UI/prefs unit) -> chip output dBm. Same mapping as
// board_tx_dbm_to_chip in main.cpp: identity except on boards with a PA
// front-end.
static inline int8_t hal_tx_dbm_to_chip(int8_t radiated) {
#if defined(BOARD_HELTEC_V4)
  return heltec_radiated_to_chip_dbm(radiated);
#else
  return radiated;
#endif
}

void radio_hal_init(SX1262* radio) {
  s_sx1262 = radio;
  s_chip = radio_chip_sx126x(radio);
}

bool radio_hal_begin() {
  if (!s_chip) return false;
  SPI_LOCK();
#if defined(BOARD_TDISPLAY_P4)
  // Antenna switch (radio_hal.h): the persisted selection, written before the
  // chip comes up.
  tdp4_exp_set(TDP4_IO_RF_SWITCH, !s_antenna_external);
  SLog.printf("[RADIO] antenna: %s (expander IO1 = %d)\n",
              s_antenna_external ? "external (MMCX)" : "internal",
              s_antenna_external ? 0 : 1);
  // The board ships in an LR2021 and an SX1262 SKU behind the same CS/BUSY
  // (RESET on expander IO16, DIO1 on IO17). The LR2021 is tried first: its
  // begin() resets the chip through the expander and verifies the LR2021
  // firmware version (RadioLib findChip, 10 tries); RADIOLIB_ERR_CHIP_NOT_FOUND
  // means the other SKU, driven as the SX1262 then is (RadioLib's SX126x
  // findChip reads the version-string register).
  delete s_chip;
  s_chip = radio_chip_lr2021(new LR2021(new Module(PIN_LORA_CS, PIN_LORA_DIO1,
                                                   PIN_LORA_RST, PIN_LORA_BUSY)));
  int16_t state = s_chip->begin();
  if (state == RADIOLIB_ERR_CHIP_NOT_FOUND) {
    SLog.printf("[RADIO] T-Display P4 SKU: no LR2021 answered (%d) -> SX1262\n", state);
    // The failed begin() ended the SPI bus (Module::term() -> SPI.end());
    // the bus is the radio's alone on this board (board_pins.h).
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    tdp4_exp_set(TDP4_IO_RADIO_RST, false);
    delay(10);
    tdp4_exp_set(TDP4_IO_RADIO_RST, true);
    delay(20);
    delete s_chip;
    s_chip = radio_chip_sx126x(s_sx1262);
    state = s_chip->begin();
  }
#else
  int16_t state = s_chip->begin();
#endif
  SLog.printf("[RADIO] %s begin() = %d %s\n", s_chip->name(), state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  SPI_UNLOCK();
  if (state != RADIOLIB_ERR_NONE) {
    // A chip that did not come up is not driven: every raw op below returns
    // its no-radio result at once. Driving it anyway makes each RadioLib
    // command wait its full SPI timeout for a BUSY line that never drops —
    // a protocol's first configuration (four commands) starved the core-0
    // idle task past the task watchdog's 5 s on the T-Display P4 while its
    // LR2021 was still driven as an SX1262, and the watchdog panic rebooted
    // the board every boot.
    SLog.println("[RADIO] chip not driven after the failed begin(): radio off");
    s_chip = nullptr;
  }
  return state == RADIOLIB_ERR_NONE;
}

bool radio_hal_config(const RadioParams& p) {
  if (!s_chip) return false;
  bool ok = true;
  SLog.printf("[RADIO] Setting freq=%.3f MHz, BW=%.0f kHz, SF=%d, CR=%d, TX=%d dBm\n",
              p.freq_mhz, p.bw_khz, p.sf, p.cr, (int)p.tx_dbm_radiated);
  SPI_LOCK();
  int16_t state = s_chip->setFrequency(p.freq_mhz);
  SLog.printf("[RADIO] setFrequency = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_chip->setBandwidth(p.bw_khz);
  SLog.printf("[RADIO] setBandwidth = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_chip->setSpreadingFactor(p.sf);
  SLog.printf("[RADIO] setSpreadingFactor = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_chip->setCodingRate(p.cr);
  SLog.printf("[RADIO] setCodingRate = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_chip->setSyncWord(p.sync_word);
  ok &= (state == RADIOLIB_ERR_NONE);
  int16_t state2 = s_chip->setPreambleLength(p.preamble_len);
  ok &= (state2 == RADIOLIB_ERR_NONE);
  SLog.printf("[RADIO] setSyncWord(0x%02X) = %d, setPreambleLength(%u) = %d\n",
              p.sync_word, state, (unsigned)p.preamble_len, state2);

  s_chip->setCRC(p.crc);

  int8_t chip = hal_tx_dbm_to_chip(p.tx_dbm_radiated);
  state = s_chip->setOutputPower(chip);
  SLog.printf("[RADIO] setOutputPower(chip %d for %d radiated) = %d %s\n",
              (int)chip, (int)p.tx_dbm_radiated,
              state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);
  SPI_UNLOCK();
  s_last_freq_mhz = p.freq_mhz;
  s_last_sf = p.sf;
  return ok;
}

bool radio_hal_set_tx_power(int8_t radiated_dbm) {
  if (!s_chip) return false;
  int8_t chip = hal_tx_dbm_to_chip(radiated_dbm);
  SPI_LOCK();
  int16_t s = s_chip->setOutputPower(chip);
  SPI_UNLOCK();
  SLog.printf("[RADIO] setOutputPower(chip %d for %d radiated) = %d %s\n",
              (int)chip, (int)radiated_dbm,
              s, s == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  return s == RADIOLIB_ERR_NONE;
}

uint8_t radio_hal_last_sf() { return s_last_sf; }

// ── Raw ops (protocol modules) ───────────────────────────────────────────────

// Armed-in-RX tracking (every transition below updates it; every protocol
// module drives the chip through these raw ops via the MeshHostApi table).
static volatile bool s_in_recv = false;

bool radio_hal_start_receive() {
  if (!s_chip) return false;
  SPI_LOCK();
  int16_t s = s_chip->startReceive();
  SPI_UNLOCK();
  s_in_recv = (s == RADIOLIB_ERR_NONE);
  if (s != RADIOLIB_ERR_NONE) {
    // The poll loop retries every tick; one line per second keeps a
    // persistent failure visible without flooding the log.
    static uint32_t last_ms = 0;
    static uint32_t suppressed = 0;
    uint32_t now = millis();
    if (now - last_ms >= 1000) {
      SLog.printf("[RADIO] startReceive = %d FAILED (%u more in the last second)\n",
                  (int)s, (unsigned)suppressed);
      last_ms = now;
      suppressed = 0;
    } else {
      suppressed++;
    }
  }
  return s == RADIOLIB_ERR_NONE;
}

bool radio_hal_in_recv() { return s_in_recv; }

uint32_t radio_hal_poll_irq() {
  if (!s_chip) return 0;
  SPI_LOCK();
  uint32_t flags = s_chip->irqFlags();
  SPI_UNLOCK();
  return flags;
}

int radio_hal_read_packet(uint8_t* buf, int max_len) {
  if (!s_chip || !buf || max_len <= 0) return -1;
  uint32_t t0 = millis();
  SPI_LOCK();
  int len = (int)s_chip->packetLength();
  if (len > max_len) len = max_len;
  int16_t rd = RADIOLIB_ERR_UNKNOWN;
  if (len > 0) rd = s_chip->readData(buf, (size_t)len);
  int r = (len > 0 && rd == RADIOLIB_ERR_NONE) ? len : -1;
  // The chip's packet status (RSSI, SNR) is kept for CRC-failed frames
  // too; read it before the re-arm starts the next reception.
  float rssi = s_chip->packetRssi();
  float snr  = s_chip->packetSnr();
  // Re-enter RX. in_recv follows the result: a failed re-arm is retried
  // from the poll loop (radio_hal_in_recv() false -> radio_hal_start_receive()).
  int16_t s = s_chip->startReceive();
  SPI_UNLOCK();
  s_in_recv = (s == RADIOLIB_ERR_NONE);
  SLog.printf("[RADIO] rx len=%d read=%d rssi=%.0f snr=%.1f rearm=%d (%u ms)\n",
              len, (int)rd, rssi, snr, (int)s, (unsigned)(millis() - t0));
  return r;
}

bool radio_hal_start_send(const uint8_t* buf, int len) {
  if (!s_chip || !buf || len <= 0) return false;
  SPI_LOCK();
  int16_t s = s_chip->startTransmit(buf, (size_t)len);
  if (s == RADIOLIB_ERR_NONE) {   // in TX until send_finished
    s_in_recv = false;
    s_tx_active = true;
    s_tx_len = len;
    s_tx_start_ms = millis();
  } else {
    SLog.printf("[RADIO] startTransmit(len=%d) = %d FAILED\n", len, (int)s);
  }
  SPI_UNLOCK();
  return s == RADIOLIB_ERR_NONE;
}

void radio_hal_send_finished() {
  if (!s_chip) return;
  uint32_t t0 = millis();
  SPI_LOCK();
  int16_t f = s_chip->finishTransmit();
  // in_recv follows the re-arm result (see radio_hal_read_packet).
  int16_t s = s_chip->startReceive();
  s_tx_active = false;
  SPI_UNLOCK();
  s_in_recv = (s == RADIOLIB_ERR_NONE);
  SLog.printf("[RADIO] tx len=%d done after %u ms: finish=%d rearm=%d (%u ms)\n",
              s_tx_len, (unsigned)(t0 - s_tx_start_ms), (int)f, (int)s,
              (unsigned)(millis() - t0));
}

float radio_hal_last_rssi() {
  if (!s_chip) return 0.0f;
  SPI_LOCK();
  float r = s_chip->packetRssi();
  SPI_UNLOCK();
  return r;
}

float radio_hal_last_snr() {
  if (!s_chip) return 0.0f;
  SPI_LOCK();
  float r = s_chip->packetSnr();
  SPI_UNLOCK();
  return r;
}

bool radio_hal_standby() {
  if (!s_chip) return false;
  SPI_LOCK();
  int16_t s = s_chip->standby();
  s_tx_active = false;
  SPI_UNLOCK();
  s_in_recv = false;
  return s == RADIOLIB_ERR_NONE;
}

bool radio_hal_sleep() {
  if (!s_chip) return false;
  SPI_LOCK();
  int16_t s = s_chip->sleep();
  s_tx_active = false;
  SPI_UNLOCK();
  s_in_recv = false;
  return s == RADIOLIB_ERR_NONE;
}

void radio_hal_set_rx_boost(bool en) {
  if (!s_chip) return;
  SPI_LOCK();
  s_chip->setRxBoost(en);
  SPI_UNLOCK();
  s_rx_boost = en;
}

bool radio_hal_rx_boost() { return s_rx_boost; }

float radio_hal_current_rssi() {
  if (!s_chip) return 0.0f;
  SPI_LOCK();
  float r = s_chip->channelRssi();   // instantaneous, not the last packet's
  SPI_UNLOCK();
  return r;
}

uint32_t radio_hal_time_on_air_ms(int len_bytes) {
  if (!s_chip || len_bytes <= 0) return 0;
  SPI_LOCK();
  uint32_t us = s_chip->timeOnAirUs((size_t)len_bytes);
  SPI_UNLOCK();
  return us / 1000;
}

// Full receiver recovery (MeshCore's sx126xResetAGC, HAL-owned; the chip
// adapter runs its own calibration sequence for the operating frequency and
// re-applies the board wiring + rx boost). Leaves the chip in standby — the
// caller re-enters RX.
void radio_hal_reset_agc() {
  if (!s_chip) return;
  SPI_LOCK();
  s_chip->recalibrate(s_last_freq_mhz, s_rx_boost);
  SPI_UNLOCK();
  s_in_recv = false;
}

// ── Antenna switch ───────────────────────────────────────────────────────────

bool radio_hal_antenna_switch() {
#if defined(BOARD_TDISPLAY_P4)
  return true;
#else
  return false;
#endif
}

bool radio_hal_antenna_external() { return s_antenna_external; }

void radio_hal_antenna_select(bool external) { s_antenna_external = external; }

const char* radio_hal_set_antenna(bool external) {
#if defined(BOARD_TDISPLAY_P4)
  // Never move the switch under a transmission: wait for the send to finish
  // (bounded above the longest LoRa packet the presets allow), then hold the
  // SPI lock so no new one starts while the line is written.
  uint32_t start = millis();
  while (s_tx_active) {
    if (millis() - start > 10000) return "radio is transmitting";
    delay(5);
  }
  SPI_LOCK();
  bool busy = s_tx_active;
  bool ok = !busy && tdp4_exp_set(TDP4_IO_RF_SWITCH, !external);
  SPI_UNLOCK();
  if (busy) return "radio is transmitting";
  if (!ok) return "expander write failed";
  s_antenna_external = external;
  SLog.printf("[RADIO] antenna: %s (expander IO1 = %d)\n",
              external ? "external (MMCX)" : "internal", external ? 0 : 1);
  return nullptr;
#else
  (void)external;
  return "no antenna switch on this board";
#endif
}

// ── Boot protocol selector ───────────────────────────────────────────────────

static char s_proto_requested[16] = "meshcore";
static char s_proto_active[16]    = "meshcore";

// Lowercase [a-z0-9_], 1..15 chars; anything else -> "meshcore".
static void sanitize_proto_id(char* out, size_t out_sz, const char* id) {
  size_t n = 0;
  if (id) {
    for (const char* c = id; *c && n < out_sz - 1; c++) {
      char ch = (char)tolower((unsigned char)*c);
      if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') {
        out[n++] = ch;
      } else {
        n = 0;
        break;
      }
    }
  }
  out[n] = '\0';
  if (n == 0) strncpy(out, "meshcore", out_sz - 1);
}

void lora_proto_set_requested(const char* id) {
  sanitize_proto_id(s_proto_requested, sizeof(s_proto_requested), id);
}

const char* lora_proto_requested() { return s_proto_requested; }
const char* lora_proto_active()    { return s_proto_active; }

void lora_proto_mark_active(const char* id) {
  sanitize_proto_id(s_proto_active, sizeof(s_proto_active), id);
}
