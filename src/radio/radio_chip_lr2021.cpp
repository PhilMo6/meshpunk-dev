// radio_chip_lr2021.cpp — RadioChip over RadioLib's LR2021 (contract:
// radio_chip.h): the T-Display P4's LR2021 SKU. Facts from the bring-up
// rungs (p4/main/rung5_radio.c; rung6_lora_rx.cpp received and transmitted
// against a T-Deck with this configuration) and LilyGo's v1 board driver:
//  - Module(cs 24, irq NC, rst NC, busy 6): RESET is XL9535 IO16, pulsed
//    here before begin() (RadioLib's reset() skips an NC pin); the board's
//    DIO1 line, LR2021 DIO11 (LilyGo's v1 InitLr2021 sets DIO11 to the IRQ
//    function), is XL9535 IO17, so there is no interrupt GPIO and the HAL
//    polls the IRQ register. begin() selects DIO11 as RadioLib's IRQ pin;
//    the standby wake reads it through the expander (power_tdisplay_p4.cpp).
//  - begin(..., tcxoVoltage 3.3) = TCXO mode at 3.3 V, the setting LilyGo's
//    v1 driver programs (lr20xx_system_set_tcxo_mode, 3.3 V, 32768 ticks;
//    RadioLib's default delay gives the same tick count). Rung 6 ran crystal
//    mode (0.0) and received and transmitted; TCXO mode at 1.6 V received
//    nothing there. At 3.3 V begin() and its Calibrate(ALL) pass and frames
//    decode (build 31 on).
//  - LilyGo's driver (lilygo_device_driver v1 driver.cpp, InitLr2021) also
//    runs the DC-DC regulator, DIO6/DIO7/DIO8/DIO10 as RF-switch outputs
//    and DIO11 as the IRQ pin. With all of them applied the P4 still
//    decoded 1 of 104 frames from two repeaters a T-Deck beside it heard at
//    -53 and -74 dBm, so this adapter leaves the regulator and the RF-switch
//    lines at RadioLib's defaults. The cause was the preamble length
//    (setPreambleLength below).
//  - IRQ flags: RADIOLIB_LR2021_IRQ_* bits translated to the SX126x
//    positions the HAL reports. A CRC-errored receive keeps its flags
//    latched until they are cleared (rung 6: startReceive() alone does
//    not clear them); readData() clears them, so CRC_ERROR is reported
//    together with RX_DONE and the HAL's read path runs, fails the read
//    and re-arms RX.

#if defined(BOARD_TDISPLAY_P4)

#include "radio_chip.h"

#include <Arduino.h>
#include <RadioLib.h>

#include "../boards/tdisplay_p4_board.h"
#include "../meshpunk_sync.h"   // SLog

class ChipLR2021 : public RadioChip {
  LR2021* r;


  static uint32_t to_sx126x(uint32_t raw) {
    uint32_t f = 0;
    if (raw & RADIOLIB_LR2021_IRQ_TX_DONE)            f |= RADIOLIB_SX126X_IRQ_TX_DONE;
    if (raw & RADIOLIB_LR2021_IRQ_RX_DONE)            f |= RADIOLIB_SX126X_IRQ_RX_DONE;
    if (raw & RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED)  f |= RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
    if (raw & RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID)  f |= RADIOLIB_SX126X_IRQ_HEADER_VALID;
    if (raw & RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR) f |= RADIOLIB_SX126X_IRQ_HEADER_ERR;
    if (raw & RADIOLIB_LR2021_IRQ_CRC_ERROR)          f |= RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_RX_DONE;
    if (raw & RADIOLIB_LR2021_IRQ_TIMEOUT)            f |= RADIOLIB_SX126X_IRQ_TIMEOUT;
    if (raw & RADIOLIB_LR2021_IRQ_CAD_DONE)           f |= RADIOLIB_SX126X_IRQ_CAD_DONE;
    if (raw & RADIOLIB_LR2021_IRQ_CAD_DETECTED)       f |= RADIOLIB_SX126X_IRQ_CAD_DETECTED;
    return f;
  }

public:
  explicit ChipLR2021(LR2021* radio) : r(radio) {}
  const char* name() const override { return "LR2021"; }

  int16_t begin() override {
    // Reset pulse through the expander (rung 6: 10 ms low, 20 ms settle).
    if (!tdp4_exp_set(TDP4_IO_RADIO_RST, false)) return RADIOLIB_ERR_UNKNOWN;
    delay(10);
    if (!tdp4_exp_set(TDP4_IO_RADIO_RST, true)) return RADIOLIB_ERR_UNKNOWN;
    delay(20);
    // IRQ output on DIO11, the pin wired to IO17; RadioLib's default is
    // DIO5, and its config() programs irqDioNum during begin().
    r->irqDioNum = 11;
    // RadioLib's LoRa defaults (434 MHz, 125 kHz, SF9, CR7, private sync
    // word, 10 dBm, 8 symbols) with TCXO mode at 3.3 V; radio_hal_config()
    // programs the protocol's parameters next.
    return r->begin(434.0f, 125.0f, 9, 7, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, 10, 8, 3.3f);
  }

  int16_t setFrequency(float mhz) override             { return r->setFrequency(mhz); }
  int16_t setBandwidth(float khz) override             { return r->setBandwidth(khz); }
  int16_t setSpreadingFactor(uint8_t sf) override      { return r->setSpreadingFactor(sf); }
  int16_t setCodingRate(uint8_t cr) override           { return r->setCodingRate(cr); }
  int16_t setSyncWord(uint8_t sync_word) override      { return r->setSyncWord(sync_word); }
  // One length for transmit and receive. The LR2021 receiver needs the
  // length the transmitters use (LR20xx datasheet rev 2.2 section 9.2.1:
  // "identical preamble length on both the transmitter and receiver sides
  // ... In cases where the preamble length is not known or can vary, it is
  // advisable to program the maximum preamble length on the receiver
  // side"); the protocol supplies its wire value (meshcore and mtlite: 16).
  int16_t setPreambleLength(uint16_t symbols) override { return r->setPreambleLength(symbols); }
  // LoRa CRC on the LR2021 is on/off: setCRC(len) with len > 0 enables it.
  int16_t setCRC(bool on) override                     { return r->setCRC(on ? 1 : 0); }
  int16_t setOutputPower(int8_t chip_dbm) override     { return r->setOutputPower(chip_dbm); }
  // RadioLib's setRxBoostedGainMode(level): "0 (disabled) to 7 (maximum
  // boost)". On = the maximum.
  int16_t setRxBoost(bool on) override                 { return r->setRxBoostedGainMode(on ? 7 : 0); }

  // RadioLib's LR2021 startReceive() (SetRxPath, SetRfFrequency,
  // SetLoraPacketParams, SetRx) is rejected by the chip while it is already
  // receiving: -706 / -707 on every re-arm after a packet read in build 32
  // (RadioLib's readData() leaves the chip in RX), and 500 times a second
  // after boot in builds 32-33, where the protocol's start and the
  // dispatcher's first recv both start receive. From standby — the state
  // finishTransmit() leaves — the same sequence never failed. Standby first
  // makes receive start from any state.
  int16_t  startReceive() override {
    r->standby();
    return r->startReceive();
  }
  uint32_t irqFlags() override                         { return to_sx126x(r->getIrqFlags()); }
  size_t   packetLength() override                     { return r->getPacketLength(); }
  int16_t  readData(uint8_t* buf, size_t len) override { return r->readData(buf, len); }
  int16_t  startTransmit(const uint8_t* buf, size_t len) override { return r->startTransmit(buf, len); }
  int16_t  finishTransmit() override                   { return r->finishTransmit(); }
  float    packetRssi() override                       { return r->getRSSI(); }
  float    packetSnr() override                        { return r->getSNR(); }
  // skipReceive: the HAL asks while the receiver is armed; without it
  // RadioLib's getRSSI(false) wraps the read in its own startReceive() and
  // standby().
  float    channelRssi() override                      { return r->getRSSI(false, true); }
  uint32_t timeOnAirUs(size_t len) override            { return (uint32_t)r->getTimeOnAir(len); }
  int16_t  standby() override                          { return r->standby(); }
  int16_t  sleep() override                            { return r->sleep(); }

  // Receiver recovery: Calibrate(ALL) in standby — the command RadioLib's
  // config() runs at begin() — then setFrequency() redoes the front-end
  // calibration for the operating frequency and the rx boost comes back.
  // Calibrate and getMod() (the BUSY wait) are below RadioLib's public API
  // (RADIOLIB_GODMODE, lib/RadioLib src/BuildOpt.h).
  void recalibrate(float freq_mhz, bool rx_boost) override {
    r->standby();
    r->calibrate(RADIOLIB_LR2021_CALIBRATE_ALL);
    Module* m = r->getMod();
    m->hal->delay(5);
    uint32_t start = millis();
    while (m->hal->digitalRead(m->getGpio())) {
      if (millis() - start > 50) break;
      m->hal->yield();
    }
    if (freq_mhz > 0.0f) r->setFrequency(freq_mhz);
    if (rx_boost) r->setRxBoostedGainMode(7);
  }
};

RadioChip* radio_chip_lr2021(LR2021* radio) { return new ChipLR2021(radio); }

#endif // BOARD_TDISPLAY_P4
