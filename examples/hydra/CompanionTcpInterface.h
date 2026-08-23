#pragma once

/* TEST SCAFFOLDING. The companion facade over TCP instead of BLE.
 *
 * The facade consumes a BaseSerialInterface and does not care what carries the
 * frames. helpers/esp32/SerialWifiInterface is already such a transport and
 * already speaks the framing the phone app's desktop tooling uses, so pointing
 * BLE_SERIAL_CLASS at this adapter lets a script on the LAN drive the facade
 * exactly as the app drives it -- CMD_APP_START, CMD_GET_CONTACTS,
 * CMD_SEND_LOGIN -- with no phone and no BLE central in the loop.
 *
 * This exists because the facade's protocol behaviour is worth exercising on
 * real hardware, and a BLE central is not always available to do it. It does
 * NOT test the BLE transport; only a phone or a BLE central does that.
 *
 * REBUILD TRAP: main.cpp pulls this in as `#include BLE_SERIAL_HEADER`, and
 * PlatformIO's dependency scanner cannot see through the macro. Editing this
 * file does NOT cause main.cpp to be recompiled, and you will flash stale code
 * and debug a firmware bug that is not there. Run `pio run -e <env> -t clean`
 * after touching it. The same applies to every *_CLASS/*_HEADER pair.
 *
 * Never ship this in a field build: it puts the companion protocol on an
 * unauthenticated TCP port. Guarded by WITH_COMPANION_TCP.
 */

#include <helpers/esp32/SerialWifiInterface.h>
#include <WiFi.h>

#ifndef COMPANION_TCP_PORT
  #define COMPANION_TCP_PORT 5000
#endif

class CompanionTcpInterface : public SerialWifiInterface {
  bool _listening;
  uint8_t _pending[MAX_FRAME_SIZE];
  size_t _pending_len;
public:
  CompanionTcpInterface() : _listening(false), _pending_len(0) { }

  /* Same shape as SerialBLEInterface::begin so BLE_SERIAL_CLASS can swap the
     two. The port cannot be opened here: setup() runs before the station has
     associated, and WiFiServer::begin() on a down netif gives a socket that
     never accepts. So it is opened lazily, once the link is up. */
  void begin(const char* prefix, char* name, uint32_t pin_code) {
    (void)prefix; (void)name; (void)pin_code;
  }

  /* CompanionFacade::loop() returns early while !isConnected(), and this
     transport only accepts its client inside checkRecvFrame -- so left alone
     the two deadlock: no accept without a connection, no connection without an
     accept. BLE does not have the problem because its accept happens in a
     stack callback. Pump it here, where the facade always calls, and hold any
     frame that falls out for the next real read. */
  void loop() override {
    if (!_listening) {
      if (WiFi.status() != WL_CONNECTED) return;
      SerialWifiInterface::begin(COMPANION_TCP_PORT);
      _listening = true;
    }
    if (!isConnected() && _pending_len == 0) {
      _pending_len = SerialWifiInterface::checkRecvFrame(_pending);
    }
  }

  size_t checkRecvFrame(uint8_t dest[]) override {
    if (!_listening) return 0;
    if (_pending_len > 0) {
      size_t n = _pending_len;
      memcpy(dest, _pending, n);
      _pending_len = 0;
      return n;
    }
    return SerialWifiInterface::checkRecvFrame(dest);
  }
};
