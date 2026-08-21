#pragma once

/* Connection slots the node reserves at stack init. One peripheral is the CLI
   and DFU port; a bridge build overrides these to add inbound peer links and
   outbound ones. Cannot be changed after the SoftDevice is enabled. */
#ifndef BLE_PRPH_SLOTS
  #define BLE_PRPH_SLOTS 1
#endif
#ifndef BLE_CENTRAL_SLOTS
  #define BLE_CENTRAL_SLOTS 0
#endif

#include "../BaseSerialInterface.h"
#include <bluefruit.h>

#ifndef BLE_TX_POWER
/* Maximum the nRF52840 supports. Was 4dBm, which is fine on a bench and not
   fine on a sited repeater: the production node sits at -65dBm from its bridge
   peer and repeatedly could not be FOUND from a laptop, while its p2p link was
   carrying traffic perfectly the whole time. Discovery is the weakest link in
   the chain, because an advert is a single unacknowledged packet -- if it is
   missed there is no retry, whereas a connection retransmits until it lands.

   +8dBm is 4dB over the old value, worth roughly 1.6x range. It costs nothing
   that matters: advertising TX duty is a few hundred microseconds per 152ms
   interval, about 0.2%, so even at 14.8mA while keyed the average is ~0.02mA
   against the ~2.3mA the scanner already draws continuously.

   BleBroadcast picks this up too -- it passes Bluefruit.getTxPower() to
   sd_ble_gap_tx_power_set for its own advertising set -- so bridge datagrams
   get the same gain. */
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
  static void onBleUartRX(uint16_t conn_handle);

public:
  /**
   * Raw SoftDevice event handler, installed via Bluefruit.setEventCallback().
   *
   * Public only so another subsystem can chain to it: that callback is a single
   * slot, so anything else needing raw events (BleBroadcast, for the bridge)
   * has to take the slot and forward what it does not consume. Dropping these
   * events is not an option -- CONN_PARAM_UPDATE_REQUEST goes unanswered and
   * the connection eventually drops.
   */
  static void onBLEEvent(ble_evt_t* evt);
private:

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
