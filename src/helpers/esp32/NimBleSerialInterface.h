#pragma once

/* The companion serial interface over NimBLE instead of Bluedroid.
 *
 * Why it exists: one call to BLEDevice::createServer() drags in about 94 KB of
 * unused ESP-BLE-MESH models through the static dispatch table of btc_task.c,
 * and Bluedroid itself is the rest. The BLE companion facade measures
 * +578,916 B on an M5 against +10,260 B on a RAK3401, and the whole of that
 * difference is the stack, not the feature.
 *
 * The wire is deliberately identical to helpers/esp32/SerialBLEInterface: the
 * same Nordic UART service and characteristic UUIDs, the same MTU, the same
 * encrypted-and-authenticated permissions, the same passkey. A phone cannot
 * tell the two apart, which is the point -- swapping the stack must not
 * require a different app or a re-pair on a fresh install.
 *
 * The two stacks cannot share a binary, so exactly one of them is compiled in.
 * Pick with -D BLE_SERIAL_CLASS=NimBleSerialInterface and put this .cpp in the
 * env's build_src_filter.
 */

#include "../BaseSerialInterface.h"
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class NimBleSerialInterface : public BaseSerialInterface,
                              public NimBLEServerCallbacks,
                              public NimBLECharacteristicCallbacks {
  NimBLEServer* pServer;
  NimBLEService* pService;
  NimBLECharacteristic* pTxCharacteristic;
  NimBLEAdvertising* pAdvertising;
  bool deviceConnected;
  bool oldDeviceConnected;
  bool _isEnabled;
  uint16_t last_conn_handle;
  uint32_t _pin_code;
  unsigned long _last_write;
  unsigned long adv_restart_time;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE  4
  StaticQueue_t recv_queue_state;
  uint8_t recv_queue_storage[FRAME_QUEUE_SIZE * sizeof(Frame)];
  QueueHandle_t recv_queue;
  int send_queue_len;
  Frame send_queue[FRAME_QUEUE_SIZE];

  void clearBuffers();

protected:
  /* NimBLEServerCallbacks. onPassKeyDisplay returns the static PIN, which is
     what setSecurityPasskey already told the stack; both are needed because
     the callback is what a DISPLAY_ONLY device answers with. */
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override;
  uint32_t onPassKeyDisplay() override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

  // NimBLECharacteristicCallbacks
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;

public:
  NimBleSerialInterface() {
    pServer = nullptr;
    pService = nullptr;
    pTxCharacteristic = nullptr;
    pAdvertising = nullptr;
    deviceConnected = false;
    oldDeviceConnected = false;
    adv_restart_time = 0;
    _isEnabled = false;
    _last_write = 0;
    last_conn_handle = 0;
    _pin_code = 0;
    recv_queue = xQueueCreateStatic(
      FRAME_QUEUE_SIZE, sizeof(Frame), recv_queue_storage, &recv_queue_state
    );
    send_queue_len = 0;
  }

  /**
   * init the BLE interface.
   * @param prefix   a prefix for the device name
   * @param name  IN/OUT - a name for the device (combined with prefix). If "@@MAC", is modified and returned
   * @param pin_code   the BLE security pin
   */
  void begin(const char* prefix, char* name, uint32_t pin_code);

  // BaseSerialInterface methods
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }

  bool isConnected() const override;

  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;
};
