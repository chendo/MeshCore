#pragma once

#include "CustomSX1262.h"
#include "RadioLibWrappers.h"
#include "SX126xReset.h"

#ifndef USE_SX1262
#define USE_SX1262
#endif

class CustomSX1262Wrapper : public RadioLibWrapper {
public:
  CustomSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    ((CustomSX1262 *)_radio)->setFrequency(freq);
    ((CustomSX1262 *)_radio)->setSpreadingFactor(sf);
    ((CustomSX1262 *)_radio)->setBandwidth(bw);
    ((CustomSX1262 *)_radio)->setCodingRate(cr);
    updatePreamble(sf);
  }

  bool isReceivingPacket() override { 
    return ((CustomSX1262 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    return ((CustomSX1262 *)_radio)->getRSSI(false);
  }
  float getLastRSSI() const override { return ((CustomSX1262 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((CustomSX1262 *)_radio)->getSNR(); }

  float packetScore(float snr, int packet_len) override {
    int sf = ((CustomSX1262 *)_radio)->spreadingFactor;
    return packetScoreInt(snr, sf, packet_len);
  }
  uint8_t getSpreadingFactor() const override { return ((CustomSX1262 *)_radio)->spreadingFactor; }
  virtual void powerOff() override {
    ((CustomSX1262 *)_radio)->sleep(false);
  }

  void doResetAGC() override { sx126xResetAGC((SX126x *)_radio); }

  bool setRxBoostedGainMode(bool en) override {
    return ((CustomSX1262 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return ((CustomSX1262 *)_radio)->getRxBoostedGainMode();
  }

  // The SX126x latches the coding rate from the header of the packet it just
  // received; RadioLib reports the raw 3-bit field, 1..4 meaning 4/5..4/8.
  // Anything else (long-interleaved codes, implicit header, a read before any
  // packet has arrived) is reported as unknown rather than guessed at.
  uint8_t getLastRxCodingRate() const override {
    uint8_t raw = 0;
    if (((CustomSX1262 *)_radio)->getLoRaRxHeaderInfo(&raw, NULL) != RADIOLIB_ERR_NONE) return 0;
    return (raw >= 1 && raw <= 4) ? (uint8_t)(4 + raw) : 0;
  }

  // Same sum RadioLib does for getTimeOnAir(), with the coding rate swapped for
  // the one asked about and every other modem parameter left as configured
  // (LDRO included — it follows from SF and BW, not from CR).
  uint32_t getEstAirtimeForCR(int len_bytes, uint8_t cr) override {
    if (cr < 5 || cr > 8) return getEstAirtimeFor(len_bytes);
    CustomSX1262* radio = (CustomSX1262 *)_radio;
    DataRate_t dr = {};
    dr.lora.spreadingFactor = radio->spreadingFactor;
    dr.lora.bandwidth       = radio->bandwidthKhz;
    dr.lora.codingRate      = cr;               // denominator, as RadioLib wants it
    PacketConfig_t pc = {};
    pc.lora.preambleLength  = radio->preambleLengthLoRa;
    pc.lora.implicitHeader  = (radio->headerType == RADIOLIB_SX126X_LORA_HEADER_IMPLICIT);
    pc.lora.crcEnabled      = (bool)radio->crcTypeLoRa;
    pc.lora.ldrOptimize     = (bool)radio->ldrOptimize;
    return radio->calculateTimeOnAir(RADIOLIB_MODEM_LORA, dr, pc, (size_t)len_bytes) / 1000;
  }
};
