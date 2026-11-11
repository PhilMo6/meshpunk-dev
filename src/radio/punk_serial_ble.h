// PunkSerialBLE — MeshCore's BaseSerialInterface over the Arduino BLE
// library's NimBLE backend: the T-Display P4, whose BLE controller is the
// ESP32-C6 behind esp-hosted (NimBLE host on the P4, HCI over SDIO). The
// service, characteristics, pairing requirements and frame queues are those
// of MeshCore's SerialBLEInterface (lib/MeshCore/src/helpers/esp32/), which
// is written against the Bluedroid callback types and remains the class of
// the S3 boards.
#pragma once

#include <sdkconfig.h>

#if BLE_COMPANION_ENABLED && defined(CONFIG_NIMBLE_ENABLED)

#include <helpers/BaseSerialInterface.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

class PunkSerialBLE : public BaseSerialInterface, BLESecurityCallbacks, BLEServerCallbacks, BLECharacteristicCallbacks {
protected:
  BLEServer *pServer;
  BLEService *pService;
  BLECharacteristic *pTxCharacteristic;
  bool deviceConnected;
  bool oldDeviceConnected;
  bool _isEnabled;
  bool _serviceStarted;
  uint16_t last_conn_id;
  uint32_t _pin_code;
  unsigned long _last_write;
  unsigned long adv_restart_time;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };
  static constexpr int kFrameQueueSize = 4;
  int recv_queue_len;
  Frame recv_queue[kFrameQueueSize];
  int send_queue_len;
  Frame send_queue[kFrameQueueSize];

  void clearBuffers() { recv_queue_len = 0; send_queue_len = 0; }

  // BLESecurityCallbacks methods
  using BLESecurityCallbacks::onAuthenticationComplete;
  uint32_t onPassKeyRequest() override;
  void onPassKeyNotify(uint32_t pass_key) override;
  bool onConfirmPIN(uint32_t pass_key) override;
  bool onSecurityRequest() override;
  void onAuthenticationComplete(ble_gap_conn_desc *desc, int status) override;

  // BLEServerCallbacks methods
  using BLEServerCallbacks::onConnect;
  using BLEServerCallbacks::onDisconnect;
  using BLEServerCallbacks::onMtuChanged;
  void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override;
  void onMtuChanged(BLEServer *server, ble_gap_conn_desc *desc, uint16_t mtu) override;
  void onDisconnect(BLEServer *server) override;

  // BLECharacteristicCallbacks methods
  using BLECharacteristicCallbacks::onWrite;
  void onWrite(BLECharacteristic *pCharacteristic, ble_gap_conn_desc *desc) override;

public:
  PunkSerialBLE();

  /**
   * init the BLE interface.
   * @param prefix   a prefix for the device name
   * @param name  IN/OUT - a name for the device (combined with prefix). If "@@MAC", is modified and returned
   * @param pin_code   the BLE security pin
   */
  void begin(const char *prefix, char *name, uint32_t pin_code);

  // BaseSerialInterface methods
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;
};

#endif  // BLE_COMPANION_ENABLED && CONFIG_NIMBLE_ENABLED
