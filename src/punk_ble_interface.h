#pragma once

#if BLE_COMPANION_ENABLED

#include <sdkconfig.h>

extern bool ble_bond_clear_pref;

#if defined(CONFIG_NIMBLE_ENABLED)

// NimBLE backend (T-Display P4: NimBLE host on the P4, controller on the
// C6): the Meshpunk serial-BLE class, NimBLE connection descriptors and
// bond store.
#include "radio/punk_serial_ble.h"
#include <host/ble_store.h>

class PunkBLEInterface : public PunkSerialBLE {
  ble_addr_t _peer_addr;
  bool _has_peer = false;

protected:
  using PunkSerialBLE::onAuthenticationComplete;
  void onAuthenticationComplete(ble_gap_conn_desc* desc, int status) override {
    if (status == 0) {
      _peer_addr = desc->peer_id_addr;
      _has_peer = true;

      // 30-50 ms interval, latency 4, 6 s timeout (units 1.25 ms / 10 ms):
      // the link parameters the Bluedroid build requests below.
      pServer->requestConnParams(desc->conn_handle, 24, 40, 4, 600);
    } else {
      // Bond mismatch — clear stale bond so next attempt re-pairs cleanly
      ble_store_util_delete_peer(&desc->peer_id_addr);
    }
    PunkSerialBLE::onAuthenticationComplete(desc, status);
  }

  using PunkSerialBLE::onDisconnect;
  void onDisconnect(BLEServer* server) override {
    if (_has_peer && ble_bond_clear_pref) {
      ble_store_util_delete_peer(&_peer_addr);
      _has_peer = false;
    }
    PunkSerialBLE::onDisconnect(server);
  }
};

#else  // Bluedroid backend (the S3 boards)

#include <helpers/esp32/SerialBLEInterface.h>

class PunkBLEInterface : public SerialBLEInterface {
  esp_bd_addr_t _peer_addr;
  bool _has_peer = false;

protected:
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    if (cmpl.success) {
      memcpy(_peer_addr, cmpl.bd_addr, sizeof(esp_bd_addr_t));
      _has_peer = true;

      esp_ble_conn_update_params_t params = {};
      memcpy(params.bda, cmpl.bd_addr, sizeof(esp_bd_addr_t));
      params.min_int = 24;   // 30ms  (units of 1.25ms)
      params.max_int = 40;   // 50ms
      params.latency = 4;    // skip up to 4 events during idle
      params.timeout = 600;  // 6s    (units of 10ms)
      esp_ble_gap_update_conn_params(&params);
    } else {
      // Bond mismatch — clear stale bond so next attempt re-pairs cleanly
      esp_ble_remove_bond_device(cmpl.bd_addr);
    }
    SerialBLEInterface::onAuthenticationComplete(cmpl);
  }

  void onDisconnect(BLEServer* pServer) override {
    if (_has_peer && ble_bond_clear_pref) {
      esp_ble_remove_bond_device(_peer_addr);
      _has_peer = false;
    }
    SerialBLEInterface::onDisconnect(pServer);
  }
};

#endif  // backend
#endif  // BLE_COMPANION_ENABLED
