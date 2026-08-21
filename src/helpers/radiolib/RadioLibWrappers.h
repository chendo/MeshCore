#pragma once

#include <Mesh.h>
#include <RadioLib.h>

#ifdef USE_CC310_HW_CRYPTO
#include <Adafruit_nRFCrypto.h>
#endif
struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  // Receive failures, divided by cause. The mixture carries the information.
  // Mostly CRC errors point to collisions. Header damage points to a signal
  // that is too weak, or has too much interference, for even the PHY header to
  // survive. Timeouts point to a preamble that never became a frame.
  uint32_t n_err_crc, n_err_header, n_err_timeout, n_err_other;
  int16_t last_recv_error;   // the RadioLib code of the most recent failure
#if RX_ERR_PAYLOAD_BYTES > 0
  // RadioLib fills the receive buffer BEFORE it reports a CRC mismatch, and it
  // says so ("to give user the option to keep them", SX126x::readData).
  // Therefore you can recover a damaged frame. In a burst, a corrupt copy of a
  // packet that arrives correctly a moment later is true evidence about which
  // packets collided.
  uint8_t last_err_payload[RX_ERR_PAYLOAD_BYTES];
  uint8_t last_err_len;
#endif
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  uint8_t _preamble_sf;

  void idle();
  void startRecv();
  void recordRecvError(int16_t err, const uint8_t* bytes, int len);
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

  /**
   * \brief  The airtime in milliseconds for a LoRa frame of 'len_bytes' at
   *        coding rate 'cr'. Every other modem parameter is as given.
   * \param  cr  the 4/x denominator. A value outside 5..8 means unknown, and
   *            the function then uses getEstAirtimeFor().
   *
   * This function uses the time-on-air sum of RadioLib. It does not keep a
   * second copy of the Semtech formula. A second copy could become different
   * from the one that the driver uses to price transmits.
  */
  uint32_t estAirtimeAtCR(int len_bytes, uint8_t cr, uint8_t sf, float bw_khz,
                          uint16_t preamble_syms, bool implicit_header, bool crc, bool ldro);

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : _radio(&radio), _board(&board), _preamble_sf(0) {
    resetStats();
  }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override {
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  uint32_t getRngSeed();
  void setTxPower(int8_t dbm);

  virtual float getCurrentRSSI() =0;
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  void updatePreamble(uint8_t sf) { _preamble_sf = sf; _radio->setPreambleLength(preambleLengthForSF(sf)); }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getRecvErrCrc() const { return n_err_crc; }
  uint32_t getRecvErrHeader() const { return n_err_header; }
  uint32_t getRecvErrTimeout() const { return n_err_timeout; }
  uint32_t getRecvErrOther() const { return n_err_other; }
  int16_t  getLastRecvError() const { return last_recv_error; }
#if RX_ERR_PAYLOAD_BYTES > 0
  const uint8_t* getLastRecvErrorPayload() const { return last_err_payload; }
  uint8_t getLastRecvErrorLen() const { return last_err_len; }
#endif
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() {
    n_recv = n_sent = n_recv_errors = 0;
    n_err_crc = n_err_header = n_err_timeout = n_err_other = 0;
    last_recv_error = 0;
#if RX_ERR_PAYLOAD_BYTES > 0
    last_err_len = 0;
#endif
  }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }

  /**
   * \brief  Decode the 3-bit coding-rate field of an explicit LoRa header.
   * \param  raw  the field as the modem reports it
   * \returns  the 4/x denominator, 5..8. Returns 0 for any value that the
   *           header does not define. Those are the reserved values, the
   *           long-interleaved codes, and a read taken before the modem has
   *           decoded a frame.
   */
  static uint8_t decodeHeaderCodingRate(uint8_t raw) {
    return (raw >= 1 && raw <= 4) ? (uint8_t)(4 + raw) : 0;
  }
  
  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) { return false; }
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
#ifdef USE_CC310_HW_CRYPTO
    nRFCrypto.Random.generate(dest, (uint16_t)sz);
    for (int i = 0; i < sz; i++) {
      dest[i] ^= _radio->randomByte() ^ (::random(0, 256) & 0xFF); // combine with Radio's entropy
    }
#else
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
#endif
  }
};
