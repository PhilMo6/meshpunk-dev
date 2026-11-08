#pragma once

#include <stdint.h>
#include <stddef.h>

// ── Radio chip adapters ─────────────────────────────────────────────────────
// What radio_hal.cpp needs from a RadioLib driver, one adapter per chip
// family the boards carry: SX1262 (T-Deck, Heltec V4, Wio Tracker L2, the
// T-Display P4's SX1262 SKU) and LR2021 (the T-Display P4's other SKU). The
// HAL keeps every policy — SPI locking, armed-in-RX tracking, logging, the
// radiated→chip dBm map, the boot protocol selector; an adapter is the
// chip's command set plus its board module wiring. Every call runs under
// the HAL's SPI_LOCK.
//
// IRQ snapshots come back in SX126x bit positions (RADIOLIB_SX126X_IRQ_*):
// lora_proto_abi.h promises the protocol modules that layout and
// punk_polled_radio.h reads it; the LR2021 adapter translates its own
// register layout into it.
class RadioChip {
public:
  virtual ~RadioChip() {}
  virtual const char* name() const = 0;

  // Chip bring-up (RadioLib begin()) plus the board's module wiring.
  // Returns the RadioLib result code.
  virtual int16_t begin() = 0;

  // RF parameters (RadioLib result codes).
  virtual int16_t setFrequency(float mhz) = 0;
  virtual int16_t setBandwidth(float khz) = 0;
  virtual int16_t setSpreadingFactor(uint8_t sf) = 0;
  virtual int16_t setCodingRate(uint8_t cr) = 0;
  virtual int16_t setSyncWord(uint8_t sync_word) = 0;
  virtual int16_t setPreambleLength(uint16_t symbols) = 0;
  virtual int16_t setCRC(bool on) = 0;
  virtual int16_t setOutputPower(int8_t chip_dbm) = 0;
  virtual int16_t setRxBoost(bool on) = 0;

  // Raw ops. readData() reads the received packet and clears the IRQ
  // flags; finishTransmit() clears them after TX_DONE and leaves standby.
  // startReceive() starts receive from any chip state (receiving,
  // standby, after a read or a transmit).
  virtual int16_t  startReceive() = 0;
  virtual uint32_t irqFlags() = 0;                               // SX126x positions
  virtual size_t   packetLength() = 0;
  virtual int16_t  readData(uint8_t* buf, size_t len) = 0;
  virtual int16_t  startTransmit(const uint8_t* buf, size_t len) = 0;
  virtual int16_t  finishTransmit() = 0;
  virtual float    packetRssi() = 0;                             // last packet
  virtual float    packetSnr() = 0;                              // last packet
  virtual float    channelRssi() = 0;                            // instantaneous, receiver armed
  virtual uint32_t timeOnAirUs(size_t len) = 0;
  virtual int16_t  standby() = 0;
  virtual int16_t  sleep() = 0;

  // Receiver recovery: redo the chip's calibrations for the operating
  // frequency, then re-apply the board wiring and the rx boost state.
  // Leaves the chip in standby.
  virtual void recalibrate(float freq_mhz, bool rx_boost) = 0;
};

class SX1262;
class LR2021;
RadioChip* radio_chip_sx126x(SX1262* radio);
#if defined(BOARD_TDISPLAY_P4)
RadioChip* radio_chip_lr2021(LR2021* radio);
#endif
