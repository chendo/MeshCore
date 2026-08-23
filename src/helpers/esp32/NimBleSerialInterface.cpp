#include "NimBleSerialInterface.h"
#include "esp_mac.h"

/* Identical to helpers/esp32/SerialBLEInterface. A phone that paired with the
   Bluedroid build must connect to this one without noticing. */
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define ADVERT_RESTART_DELAY  1000   // millis
#define BLE_WRITE_MIN_INTERVAL 60

#ifndef BLE_DEBUG_LOGGING
  #define BLE_DEBUG_LOGGING 0
#endif
#if BLE_DEBUG_LOGGING
  #define NB_DEBUG(F, ...) Serial.printf("BLE: " F "\n", ##__VA_ARGS__)
#else
  #define NB_DEBUG(...) {}
#endif

void NimBleSerialInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
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

  NimBLEDevice::init(dev_name);
  NimBLEDevice::setMTU(MAX_FRAME_SIZE);

  /* Bluedroid asked for ESP_LE_AUTH_REQ_SC_MITM_BOND: secure connections, man
     in the middle protection, and bonding. Same three here. DISPLAY_ONLY with
     a static passkey is what makes the phone prompt for the PIN. */
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(pin_code);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(this);

  pService = pServer->createService(SERVICE_UUID);

  /* READ_ENC|READ_AUTHEN is NimBLE's spelling of ESP_GATT_PERM_READ_ENC_MITM.
     NimBLE creates the CCCD itself for a NOTIFY characteristic, so unlike
     Bluedroid there is no BLE2902 descriptor to add. */
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
    NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);

  NimBLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  pRxCharacteristic->setCallbacks(this);

  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
}

// -------- NimBLEServerCallbacks

uint32_t NimBleSerialInterface::onPassKeyDisplay() {
  NB_DEBUG("onPassKeyDisplay()");
  return _pin_code;
}

void NimBleSerialInterface::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
  NB_DEBUG("auth complete, encrypted=%d authenticated=%d",
           (int)connInfo.isEncrypted(), (int)connInfo.isAuthenticated());
  if (!connInfo.isEncrypted()) {
    // Not encrypted: drop it rather than serve the CLI in the clear.
    pServer->disconnect(connInfo.getConnHandle());
  }
}

void NimBleSerialInterface::onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) {
  last_conn_handle = connInfo.getConnHandle();
  deviceConnected = true;
  NB_DEBUG("onConnect(), handle=%d", (int)last_conn_handle);
}

void NimBleSerialInterface::onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) {
  NB_DEBUG("onMTUChange(), mtu=%d", (int)MTU);
}

void NimBleSerialInterface::onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) {
  NB_DEBUG("onDisconnect(), reason=%d", reason);
  deviceConnected = false;
  if (_isEnabled) {
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- NimBLECharacteristicCallbacks

void NimBleSerialInterface::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
  NimBLEAttValue val = pCharacteristic->getValue();
  int len = val.length();

  if (len > MAX_FRAME_SIZE) {
    NB_DEBUG("ERROR: onWrite(), frame too big, len=%d", len);
  } else {
    Frame frame = {};
    frame.len = len;
    memcpy(frame.buf, val.data(), len);

    /* This runs on the NimBLE host task, so the queue is the handover to
       loop(). Same reason Bluedroid's version uses one. */
    if (xQueueSend(recv_queue, &frame, 0) != pdTRUE) {
      NB_DEBUG("ERROR: onWrite(), recv_queue is full!");
    }
  }
}

// ---------- public methods

void NimBleSerialInterface::clearBuffers() {
  xQueueReset(recv_queue);
  send_queue_len = 0;
}

void NimBleSerialInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  pService->start();
  pAdvertising->start();
  adv_restart_time = 0;
}

void NimBleSerialInterface::disable() {
  _isEnabled = false;

  NB_DEBUG("disable()");

  pAdvertising->stop();
  if (deviceConnected) pServer->disconnect(last_conn_handle);
  oldDeviceConnected = deviceConnected = false;
  adv_restart_time = 0;
}

size_t NimBleSerialInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    NB_DEBUG("writeFrame(), frame too big, len=%d", (int)len);
    return 0;
  }

  if (deviceConnected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      NB_DEBUG("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

bool NimBleSerialInterface::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t NimBleSerialInterface::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    _last_write = millis();
    pTxCharacteristic->setValue(send_queue[0].buf, send_queue[0].len);
    pTxCharacteristic->notify();

    NB_DEBUG("writeBytes: sz=%d, hdr=%d", (int)send_queue[0].len, (int)send_queue[0].buf[0]);

    send_queue_len--;
    for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
      send_queue[i] = send_queue[i + 1];
    }
  }

  Frame frame;
  if (xQueueReceive(recv_queue, &frame, 0) == pdTRUE) {
    memcpy(dest, frame.buf, frame.len);
    NB_DEBUG("readBytes: sz=%d, hdr=%d", (int)frame.len, (int)dest[0]);
    return frame.len;
  }

  if (deviceConnected != oldDeviceConnected) {
    if (!deviceConnected) {    // disconnecting
      clearBuffers();
      NB_DEBUG("disconnecting...");
      adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    } else {
      NB_DEBUG("connecting, stopping advertising");
      pAdvertising->stop();
      adv_restart_time = 0;
    }
    oldDeviceConnected = deviceConnected;
  }

  if (adv_restart_time && millis() >= adv_restart_time) {
    if (pServer->getConnectedCount() == 0) {
      NB_DEBUG("re-starting advertising");
      pAdvertising->start();
    }
    adv_restart_time = 0;
  }
  return 0;
}

bool NimBleSerialInterface::isConnected() const {
  return deviceConnected;
}
