#include "punk_serial_ble.h"

#if BLE_COMPANION_ENABLED && defined(CONFIG_NIMBLE_ENABLED)

#include <esp_mac.h>

// The MeshCore companion's Nordic-UART-style service (SerialBLEInterface.cpp)
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define ADVERT_RESTART_DELAY   1000   // millis
#define BLE_WRITE_MIN_INTERVAL 60     // millis between notifications

PunkSerialBLE::PunkSerialBLE() {
  pServer = NULL;
  pService = NULL;
  pTxCharacteristic = NULL;
  deviceConnected = false;
  oldDeviceConnected = false;
  _isEnabled = false;
  _serviceStarted = false;
  last_conn_id = 0;
  _pin_code = 0;
  _last_write = 0;
  adv_restart_time = 0;
  send_queue_len = recv_queue_len = 0;
}

void PunkSerialBLE::begin(const char* prefix, char* name, uint32_t pin_code) {
  _pin_code = pin_code;

  if (strcmp(name, "@@MAC") == 0) {
    uint8_t addr[8];
    memset(addr, 0, sizeof(addr));
    esp_efuse_mac_get_default(addr);
    sprintf(name, "%02X%02X%02X%02X%02X%02X",    // modify (IN-OUT param)
            addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  char dev_name[32+16];
  sprintf(dev_name, "%s%s", prefix, name);

  // The hosted controller (C6) and the NimBLE host come up inside init().
  BLEDevice::init(dev_name);
  BLEDevice::setSecurityCallbacks(this);
  BLEDevice::setMTU(MAX_FRAME_SIZE);

  // Fixed passkey shown by this side (display-only I/O capability), typed
  // on the phone: bonded, MITM-protected, secure-connections pairing.
  BLESecurity sec;
  sec.setPassKey(true, pin_code);
  sec.setCapability(ESP_IO_CAP_OUT);
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(this);
  pService = pServer->createService(SERVICE_UUID);

  // Both values need an authenticated (MITM) link. NimBLE takes that as
  // characteristic flags; setAccessPermissions() only acts on Bluedroid.
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_READ_AUTHEN | BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  pRxCharacteristic->setCallbacks(this);

  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
}

// -------- BLESecurityCallbacks methods

uint32_t PunkSerialBLE::onPassKeyRequest() {
  return _pin_code;
}

void PunkSerialBLE::onPassKeyNotify(uint32_t pass_key) {
}

bool PunkSerialBLE::onConfirmPIN(uint32_t pass_key) {
  return true;
}

bool PunkSerialBLE::onSecurityRequest() {
  return true;  // allow
}

void PunkSerialBLE::onAuthenticationComplete(ble_gap_conn_desc* desc, int status) {
  if (status == 0) {
    deviceConnected = true;
  } else {
    pServer->disconnect(desc->conn_handle);
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLEServerCallbacks methods

void PunkSerialBLE::onConnect(BLEServer* server, ble_gap_conn_desc* desc) {
  last_conn_id = desc->conn_handle;
}

void PunkSerialBLE::onMtuChanged(BLEServer* server, ble_gap_conn_desc* desc, uint16_t mtu) {
}

void PunkSerialBLE::onDisconnect(BLEServer* server) {
  if (_isEnabled) {
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    // checkRecvFrame() sees the connection count at 0 and clears deviceConnected
  }
}

// -------- BLECharacteristicCallbacks methods

void PunkSerialBLE::onWrite(BLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) {
  uint8_t* rxValue = pCharacteristic->getData();
  int len = pCharacteristic->getLength();
  if (len > MAX_FRAME_SIZE || recv_queue_len >= kFrameQueueSize) {
    return;   // dropped, as SerialBLEInterface drops them
  }
  recv_queue[recv_queue_len].len = len;
  memcpy(recv_queue[recv_queue_len].buf, rxValue, len);
  recv_queue_len++;
}

// ---------- public methods

void PunkSerialBLE::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  // NimBLE registers a service once and cannot stop it; the first
  // advertising start also starts the GATT server.
  if (!_serviceStarted) {
    pService->start();
    _serviceStarted = true;
  }
  pServer->getAdvertising()->start();
  adv_restart_time = 0;
}

void PunkSerialBLE::disable() {
  _isEnabled = false;
  pServer->getAdvertising()->stop();
  if (pServer->getConnectedCount() > 0) pServer->disconnect(last_conn_id);
  oldDeviceConnected = deviceConnected = false;
  adv_restart_time = 0;
}

size_t PunkSerialBLE::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) return 0;

  if (deviceConnected && len > 0) {
    if (send_queue_len >= kFrameQueueSize) return 0;   // send queue full

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;
    return len;
  }
  return 0;
}

bool PunkSerialBLE::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t PunkSerialBLE::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    _last_write = millis();
    pTxCharacteristic->setValue(send_queue[0].buf, send_queue[0].len);
    pTxCharacteristic->notify();

    send_queue_len--;
    for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
      send_queue[i] = send_queue[i + 1];
    }
  }

  if (recv_queue_len > 0) {   // check recv queue
    size_t len = recv_queue[0].len;   // take from top of queue
    memcpy(dest, recv_queue[0].buf, len);

    recv_queue_len--;
    for (int i = 0; i < recv_queue_len; i++) {   // delete top item from queue
      recv_queue[i] = recv_queue[i + 1];
    }
    return len;
  }

  if (pServer->getConnectedCount() == 0)  deviceConnected = false;

  if (deviceConnected != oldDeviceConnected) {
    if (!deviceConnected) {    // disconnecting
      clearBuffers();
      adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    } else {                   // connecting
      pServer->getAdvertising()->stop();
      adv_restart_time = 0;
    }
    oldDeviceConnected = deviceConnected;
  }

  if (adv_restart_time && millis() >= adv_restart_time) {
    if (pServer->getConnectedCount() == 0) {
      pServer->getAdvertising()->start();  // re-Start advertising
    }
    adv_restart_time = 0;
  }
  return 0;
}

bool PunkSerialBLE::isConnected() const {
  return deviceConnected;
}

#endif  // BLE_COMPANION_ENABLED && CONFIG_NIMBLE_ENABLED
