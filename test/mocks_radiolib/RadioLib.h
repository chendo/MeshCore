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

enum ModemType_t { RADIOLIB_MODEM_FSK = 0, RADIOLIB_MODEM_LORA, RADIOLIB_MODEM_LRFHSS };

struct LoRaRate_t { uint8_t spreadingFactor; float bandwidth; uint8_t codingRate; };
union DataRate_t { LoRaRate_t lora; };

struct LoRaPacketConfig_t { uint16_t preambleLength; bool implicitHeader, crcEnabled, ldrOptimize; };
union PacketConfig_t { LoRaPacketConfig_t lora; };

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

  // The modem settings getTimeOnAir() answers for. Defaults are MeshCore's own
  // preset: SF8, 62.5 kHz, 4/5, 32-symbol preamble, explicit header, CRC on.
  uint8_t  cfg_sf = 8;
  float    cfg_bw_khz = 62.5f;
  uint8_t  cfg_cr = 5;               // 4/x denominator
  uint16_t cfg_preamble = 32;
  bool     cfg_implicit_header = false, cfg_crc = true, cfg_ldro = false;

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
  // Semtech's time-on-air, section 4.1.1.7 of the SX1276 datasheet and 6.1.4 of
  // the SX1268's -- written out longhand rather than lifted from RadioLib, so
  // that agreeing with the driver is evidence and not a tautology.
  virtual RadioLibTime_t calculateTimeOnAir(ModemType_t modem, DataRate_t dr, PacketConfig_t pc, size_t len) {
    if (modem != RADIOLIB_MODEM_LORA) return 0;
    double sym_us = (double)(1u << dr.lora.spreadingFactor) * 1000.0 / dr.lora.bandwidth;
    int de = pc.lora.ldrOptimize ? 1 : 0;
    int num = 8 * (int)len - 4 * dr.lora.spreadingFactor + 28
              + (pc.lora.crcEnabled ? 16 : 0) - (pc.lora.implicitHeader ? 20 : 0);
    int den = 4 * (dr.lora.spreadingFactor - 2 * de);
    int coded = num <= 0 ? 0 : ((num + den - 1) / den) * dr.lora.codingRate;
    double syms = (double)pc.lora.preambleLength + 4.25 + 8.0 + coded;
    return (RadioLibTime_t)(sym_us * syms);
  }

  virtual RadioLibTime_t getTimeOnAir(size_t len) {
    DataRate_t dr = {};
    dr.lora.spreadingFactor = cfg_sf;
    dr.lora.bandwidth = cfg_bw_khz;
    dr.lora.codingRate = cfg_cr;
    PacketConfig_t pc = {};
    pc.lora.preambleLength = cfg_preamble;
    pc.lora.implicitHeader = cfg_implicit_header;
    pc.lora.crcEnabled = cfg_crc;
    pc.lora.ldrOptimize = cfg_ldro;
    return calculateTimeOnAir(RADIOLIB_MODEM_LORA, dr, pc, len);
  }
  virtual float getRSSI() { return rssi; }
  virtual float getSNR() { return snr; }
  virtual int32_t random(int32_t max_val) { return max_val / 2; }
  virtual uint8_t randomByte() { return 0x42; }
};
