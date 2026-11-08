// radio_chip_sx126x.cpp — RadioChip over RadioLib's SX1262 (contract:
// radio_chip.h): the T-Deck, Heltec V4 and Wio Tracker L2 radio, and the
// T-Display P4's SX1262 SKU. The board module wiring is applied in begin()
// and re-applied by recalibrate(). The SX1262 object itself is main.cpp's
// (RADIO_CLASS radio).

#include "radio_chip.h"

#include <Arduino.h>
#include <RadioLib.h>

#include "../meshpunk_sync.h"   // SLog

class ChipSX126x : public RadioChip {
  SX1262* r;

  // Module wiring on the boards whose SX1262 is not wired like RadioLib's
  // defaults: a TCXO on DIO3 (Heltec V4 1.8 V; Wio L2 3.0 V, the Meshtastic
  // seeed_wio_tracker_L2 variant's SX126X_DIO3_TCXO_VOLTAGE), the RF switch
  // (Heltec: the FEM's TX/RX switch) driven from DIO2, and a 140 mA
  // over-current limit — the value SetPaConfig applies for the SX1262's
  // high-power PA, which RadioLib's begin() replaces with 60 mA (the Heltec
  // PA has its own LDO). The T-Deck and the P4's SX1262 SKU run begin()'s
  // defaults.
  void applyBoardWiring() {
#if defined(BOARD_HELTEC_V4) || defined(BOARD_WIO_L2)
#if defined(BOARD_HELTEC_V4)
    const float tcxo_v = 1.8f;
#else
    const float tcxo_v = 3.0f;
#endif
    int16_t s = r->setTCXO(tcxo_v);
    SLog.printf("[RADIO] setTCXO(%.1f) = %d %s\n", tcxo_v, s,
                s == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
    s = r->setDio2AsRfSwitch(true);
    SLog.printf("[RADIO] setDio2AsRfSwitch = %d %s\n", s,
                s == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
    r->setCurrentLimit(140);
#endif
  }

public:
  explicit ChipSX126x(SX1262* radio) : r(radio) {}
  const char* name() const override { return "SX1262"; }

  int16_t begin() override {
    int16_t state = r->begin();
    if (state == RADIOLIB_ERR_NONE) applyBoardWiring();
    return state;
  }

  int16_t setFrequency(float mhz) override             { return r->setFrequency(mhz); }
  int16_t setBandwidth(float khz) override             { return r->setBandwidth(khz); }
  int16_t setSpreadingFactor(uint8_t sf) override      { return r->setSpreadingFactor(sf); }
  int16_t setCodingRate(uint8_t cr) override           { return r->setCodingRate(cr); }
  int16_t setSyncWord(uint8_t sync_word) override      { return r->setSyncWord(sync_word); }
  int16_t setPreambleLength(uint16_t symbols) override { return r->setPreambleLength(symbols); }
  // LoRa CRC is on/off; 1/0 exactly as the pre-HAL code's setCRC(true).
  int16_t setCRC(bool on) override                     { return r->setCRC(on ? 1 : 0); }
  int16_t setOutputPower(int8_t chip_dbm) override     { return r->setOutputPower(chip_dbm); }
  int16_t setRxBoost(bool on) override                 { return r->setRxBoostedGainMode(on, true); }

  int16_t  startReceive() override                     { return r->startReceive(); }
  uint32_t irqFlags() override                         { return r->getIrqFlags(); }
  size_t   packetLength() override                     { return r->getPacketLength(); }
  int16_t  readData(uint8_t* buf, size_t len) override { return r->readData(buf, len); }
  int16_t  startTransmit(const uint8_t* buf, size_t len) override { return r->startTransmit(buf, len); }
  int16_t  finishTransmit() override                   { return r->finishTransmit(); }
  float    packetRssi() override                       { return r->getRSSI(); }
  float    packetSnr() override                        { return r->getSNR(); }
  float    channelRssi() override                      { return r->getRSSI(false); }
  uint32_t timeOnAirUs(size_t len) override            { return (uint32_t)r->getTimeOnAir(len); }
  int16_t  standby() override                          { return r->standby(); }
  int16_t  sleep() override                            { return r->sleep(); }

  // MeshCore's sx126xResetAGC: warm sleep powers down the analog front end,
  // Calibrate(ALL) refreshes the ADC/PLL/image calibration, then the image
  // calibration is redone for the operating band (Calibrate defaults it to
  // 902-928 MHz) and the wiring + rx boost the calibration cleared come
  // back. Calibrate goes through Module (RADIOLIB_GODMODE, lib/RadioLib
  // src/BuildOpt.h).
  void recalibrate(float freq_mhz, bool rx_boost) override {
    r->sleep(true);
    r->standby(RADIOLIB_SX126X_STANDBY_RC, true);
    uint8_t calData = RADIOLIB_SX126X_CALIBRATE_ALL;
    r->mod->SPIwriteStream(RADIOLIB_SX126X_CMD_CALIBRATE, &calData, 1, true, false);
    r->mod->hal->delay(5);
    uint32_t start = millis();
    while (r->mod->hal->digitalRead(r->mod->getGpio())) {
      if (millis() - start > 50) break;
      r->mod->hal->yield();
    }
    if (freq_mhz > 0.0f) r->calibrateImage(freq_mhz);
#if defined(BOARD_HELTEC_V4) || defined(BOARD_WIO_L2)
    r->setDio2AsRfSwitch(true);
#endif
    if (rx_boost) r->setRxBoostedGainMode(true, true);
  }
};

RadioChip* radio_chip_sx126x(SX1262* radio) { return new ChipSX126x(radio); }
