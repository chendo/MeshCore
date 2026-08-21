#pragma once

#include "../BaseSerialInterface.h"
#include <bluefruit.h>

// Discovery is the weak link, not the link itself: an advert is a single unacked packet
// with no retry, while an established connection just retransmits until it lands. A node
// at -65 dBm carried its traffic perfectly yet could not be found by a scanning laptop.
// +8 dBm (the nRF52840 max) buys 4 dB, ~1.6x range, for nothing that matters - the PA is
// keyed a few hundred microseconds per 152 ms advert interval, ~0.02 mA averaged, against
// the ~2.3 mA the scanner on the other end burns continuously.
#ifndef BLE_TX_POWER
#define BLE_TX_POWER 8
#endif

class SerialBLEInterface : public BaseSerialInterface {
  BLEDfu bledfu;
  BLEUart bleuart;
  bool _isEnabled;
  bool _isDeviceConnected;
  uint16_t _conn_handle;
  unsigned long _last_health_check;
  unsigned long _last_retry_attempt;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE  12
  
  uint8_t send_queue_len;
  Frame send_queue[FRAME_QUEUE_SIZE];
  
  uint8_t recv_queue_len;
  Frame recv_queue[FRAME_QUEUE_SIZE];

  void clearBuffers();
  void shiftSendQueueLeft();
  void shiftRecvQueueLeft();
  bool isValidConnection(uint16_t handle, bool requireWaitingForSecurity = false) const;
  bool isAdvertising() const;
  static void onConnect(uint16_t connection_handle);
  static void onDisconnect(uint16_t connection_handle, uint8_t reason);
  static void onSecured(uint16_t connection_handle);
  static bool onPairingPasskey(uint16_t connection_handle, uint8_t const passkey[6], bool match_request);
  static void onPairingComplete(uint16_t connection_handle, uint8_t auth_status);
  static void onBLEEvent(ble_evt_t* evt);
  static void onBleUartRX(uint16_t conn_handle);

public:
  SerialBLEInterface() {
    _isEnabled = false;
    _isDeviceConnected = false;
    _conn_handle = BLE_CONN_HANDLE_INVALID;
    _last_health_check = 0;
    _last_retry_attempt = 0;
    send_queue_len = 0;
    recv_queue_len = 0;
  }

  /**
   * init the BLE interface.
   * @param prefix   a prefix for the device name
   * @param name  IN/OUT - a name for the device (combined with prefix). If "@@MAC", is modified and returned
   * @param pin_code   the BLE security pin
   */
  void begin(const char* prefix, char* name, uint32_t pin_code);

  void disconnect();
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;
};

#if BLE_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define BLE_DEBUG_PRINT(F, ...) Serial.printf("BLE: " F, ##__VA_ARGS__)
  #define BLE_DEBUG_PRINTLN(F, ...) Serial.printf("BLE: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_DEBUG_PRINT(...) {}
  #define BLE_DEBUG_PRINTLN(...) {}
#endif
