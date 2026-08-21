#pragma once

// Test stand-in for RadioLib, big enough to build RadioLibWrappers.cpp on the
// host. Only what that file touches is here; the error codes are copied from
// RadioLib's TypeDef.h and must stay in step with it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// RadioLib.h drags in Arduino.h in a firmware build; these two are all that
// RadioLibWrappers.cpp gets from it.
using std::max;
using std::min;
inline long random(long lo, long hi) { return lo + (hi - lo) / 2; }

#define RADIOLIB_ERR_NONE                  (0)
#define RADIOLIB_ERR_RX_TIMEOUT            (-6)
#define RADIOLIB_ERR_CRC_MISMATCH          (-7)
#define RADIOLIB_PREAMBLE_DETECTED         (-14)
#define RADIOLIB_CHANNEL_FREE              (-15)
#define RADIOLIB_ERR_WRONG_MODEM           (-20)
#define RADIOLIB_ERR_LORA_HEADER_DAMAGED   (-24)

typedef uint32_t RadioLibTime_t;

class PhysicalLayer {
public:
  virtual ~PhysicalLayer() = default;

  // --- driven by the test ---
  void (*packet_action)(void) = nullptr;
  int16_t read_data_result = RADIOLIB_ERR_NONE;   // what readData() reports
  size_t  packet_length = 0;                      // what getPacketLength() reports
  uint8_t packet_bytes[256] = {0};                // what readData() delivers
  float   rssi = -100.0f, snr = 5.0f;
  int16_t scan_result = RADIOLIB_CHANNEL_FREE;
  RadioLibTime_t time_on_air = 100000;

  // Stand in for the DIO interrupt: RadioLibWrapper installs its own ISR in
  // begin(), and its receive path only runs once that ISR has fired.
  void firePacketReceived() { if (packet_action) packet_action(); }
  void deliver(const uint8_t* bytes, size_t len, int16_t result) {
    memcpy(packet_bytes, bytes, len);
    packet_length = len;
    read_data_result = result;
    firePacketReceived();
  }

  // --- the surface RadioLibWrappers.cpp uses ---
  virtual void setPacketReceivedAction(void (*func)(void)) { packet_action = func; }
  virtual int16_t setPreambleLength(size_t) { return RADIOLIB_ERR_NONE; }
  virtual int16_t sleep() { return RADIOLIB_ERR_NONE; }
  virtual int16_t standby() { return RADIOLIB_ERR_NONE; }
  virtual int16_t setOutputPower(int8_t) { return RADIOLIB_ERR_NONE; }
  virtual int16_t startReceive() { return RADIOLIB_ERR_NONE; }
  virtual int16_t startTransmit(const uint8_t*, size_t) { return RADIOLIB_ERR_NONE; }
  virtual int16_t finishTransmit() { return RADIOLIB_ERR_NONE; }
  virtual int16_t scanChannel() { return scan_result; }
  virtual size_t getPacketLength() { return packet_length; }
  virtual int16_t readData(uint8_t* data, size_t len) {
    memcpy(data, packet_bytes, len);
    return read_data_result;
  }
  virtual RadioLibTime_t getTimeOnAir(size_t) { return time_on_air; }
  virtual float getRSSI() { return rssi; }
  virtual float getSNR() { return snr; }
  virtual int32_t random(int32_t max_val) { return max_val / 2; }
  virtual uint8_t randomByte() { return 0x42; }
};
