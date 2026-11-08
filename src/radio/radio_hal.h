#pragma once

#include <stdint.h>

// ── Radio HAL ────────────────────────────────────────────────────────────────
// The ONE access path to the LoRa chip, whichever LoRa protocol is active:
// bring-up, parameter programming, and the raw polled TX/RX primitives.
// Protocol modules call these through the MeshHostApi table. Every call
// takes SPI_LOCK (the bus is shared with TFT + SD on the T-Deck). The chip
// itself sits behind a RadioChip adapter (radio_chip.h): the SX1262 on the
// S3 boards and the T-Display P4's SX1262 SKU, the LR2021 on the P4's
// other SKU — IRQ snapshots are always in SX126x bit positions.
//
// This header is MeshCore-free on purpose: protocol modules compile against
// it (via the MeshHostApi function table), and the record of what the chip
// is programmed with must not depend on MeshCore headers.

class SX1262;   // RadioLib; CustomSX1262 derives from it

struct RadioParams {
  float    freq_mhz;
  float    bw_khz;
  uint8_t  sf;
  uint8_t  cr;
  int8_t   tx_dbm_radiated;  // radiated dBm (UI/prefs unit); boards with a PA
                             // front-end map it to chip dBm inside the HAL
  bool     crc;
  uint8_t  sync_word;        // LoRa sync word byte (0x12 private, 0x2B Meshtastic)
  uint16_t preamble_len;     // preamble symbols
};

// The values the chip ran with before the HAL existed: RadioLib's begin()
// defaults, now programmed explicitly so they are visible and settable.
#define RADIO_HAL_SYNC_WORD_DEFAULT 0x12
#define RADIO_HAL_PREAMBLE_DEFAULT  8

// Bind the HAL to the SX1262 object (constructed in main.cpp). Call once in
// setup() before radio_hal_begin().
void radio_hal_init(SX1262* radio);

// Chip bring-up: the adapter's begin() (RadioLib begin() plus the board's
// module wiring — Heltec V4: 1.8V TCXO on DIO3, DIO2 as RF switch, 140mA
// current limit; Wio L2: the same at 3.0 V). On the T-Display P4 the LR2021
// is tried first and the SX1262 when no LR2021 answers (the two SKUs).
// Returns false if begin() failed (logged either way); the chip is then not
// driven at all.
bool radio_hal_begin();

// Program all RF parameters. Logs each RadioLib result; returns true only if
// every call returned RADIOLIB_ERR_NONE. Leaves the chip in standby — the
// caller re-enters RX (MeshCore: the dispatcher's next recvRaw; other
// protocols: radio_hal_start_receive()).
bool radio_hal_config(const RadioParams& p);

// ── Raw ops (every protocol path) ────────────────────────────────────────────
// Polled model: no ISRs. The protocol's loop() (mesh_task, Core 1, 2ms
// cadence) calls poll_irq() and reacts to RX_DONE/TX_DONE flags.
bool     radio_hal_start_receive();
// Chip armed in RX (HAL-tracked; every chip access flows through these raw
// ops, so this is the standby loop's RX check under any protocol).
bool     radio_hal_in_recv();
uint32_t radio_hal_poll_irq();                            // SX126x IRQ flag snapshot
int      radio_hal_read_packet(uint8_t* buf, int max_len); // after RX_DONE; re-arms RX; -1 on error
bool     radio_hal_start_send(const uint8_t* buf, int len);
void     radio_hal_send_finished();                       // after TX_DONE: finishTransmit + re-enter RX
float    radio_hal_last_rssi();
float    radio_hal_last_snr();
float    radio_hal_current_rssi();       // instantaneous channel RSSI (CSMA / noise floor)
uint32_t radio_hal_time_on_air_ms(int len_bytes);
uint8_t  radio_hal_last_sf();            // as programmed by the last config
bool     radio_hal_standby();
bool     radio_hal_sleep();
void     radio_hal_set_rx_boost(bool en);
bool     radio_hal_rx_boost();           // last state given to set_rx_boost
bool     radio_hal_set_tx_power(int8_t radiated_dbm);
// Full receiver recovery: warm sleep + Calibrate(ALL) + image recalibration
// for the operating band + board wiring/rx-boost re-apply. Leaves standby —
// the caller re-enters RX.
void     radio_hal_reset_agc();

// ── Antenna switch (boards with one) ─────────────────────────────────────────
// T-Display P4: SKY13453 VCTL on expander IO1, 1 = the internal antenna, 0 =
// the MMCX connector (LilyGo v1 driver SetSky13453RfSwitch). Boards without
// a switch: antenna_switch() false, antenna_external() false, set_antenna()
// refused. The selection is the firmware pref antenna=internal|external
// (main.cpp): antenna_select() records the loaded pref, radio_hal_begin()
// writes it before the chip comes up, set_antenna() writes a change at once.
bool        radio_hal_antenna_switch();        // the board has a switch
bool        radio_hal_antenna_external();      // current selection
void        radio_hal_antenna_select(bool external);
// Select the antenna now. Returns nullptr when done, otherwise the reason it
// was not: no switch on this board, a transmission still on the air after a
// bounded wait (the PA must not see the switch move), or a failed write.
const char* radio_hal_set_antenna(bool external);

// ── Boot protocol selector ───────────────────────────────────────────────────
// `requested` is the persisted user choice (firmware_prefs lora_protocol=...);
// `active` is what this boot actually runs after the install/load checks
// (a failed load runs "none" — the radio-off floor, never a substitute).
// Ids are lowercase [a-z0-9_], max 15 chars; anything else is replaced with
// "meshcore" at set time.
void        lora_proto_set_requested(const char* id);
const char* lora_proto_requested();
const char* lora_proto_active();
void        lora_proto_mark_active(const char* id);
