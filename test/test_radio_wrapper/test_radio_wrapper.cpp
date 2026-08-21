#include <gtest/gtest.h>

#include "helpers/radiolib/RadioLibWrappers.h"

// This is a small concrete wrapper. RadioLibWrapper leaves three functions
// abstract. These tests do not use any of the three.
class TestWrapper : public RadioLibWrapper {
public:
  TestWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) {}
  bool isReceivingPacket() override { return false; }
  float getCurrentRSSI() override { return -120.0f; }
  void setParams(float, float, uint8_t, uint8_t) override {}

  // This does what CustomSX1262Wrapper::getEstAirtimeForCR() does. It does not
  // do the direct register reads that only a real SX126x can do. The modem
  // settings come from the replacement radio. Thus this test covers the shared
  // helper function.
  uint32_t estAirtimeAtCRHere(int len, uint8_t cr, PhysicalLayer& r) {
    return estAirtimeAtCR(len, cr, r.cfg_sf, r.cfg_bw_khz, r.cfg_preamble,
                          r.cfg_implicit_header, r.cfg_crc, r.cfg_ldro);
  }
};

class TestBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 4200; }
  const char* getManufacturerName() const override { return "test"; }
  void reboot() override {}
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

TEST(CodingRateDecode, DefinedFieldValuesBecomeTheDenominator) {
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(1), 5);
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(2), 6);
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(3), 7);
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(4), 8);
}

// 0 means "the radio has decoded no frame yet". The LoRa header reserves 5 to
// 7. To report any of them as a coding rate is a guess. A caller cannot tell a
// guess from a true reading.
TEST(CodingRateDecode, EverythingElseIsUnknown) {
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(0), 0);
  for (int raw = 5; raw <= 255; raw++) {
    EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate((uint8_t)raw), 0) << "raw=" << raw;
  }
}

// A radio that cannot read the field must say so. It must not use our own
// coding rate. In explicit-header mode the value belongs to the sender.
TEST(CodingRateDecode, TheBaseRadioReportsUnknown) {
  PhysicalLayer radio;
  TestBoard board;
  TestWrapper w(radio, board);
  EXPECT_EQ(w.getLastRxCodingRate(), 0);
}

class RecvErrors : public ::testing::Test {
protected:
  PhysicalLayer radio;
  TestBoard board;
  TestWrapper w{radio, board};
  uint8_t buf[MAX_TRANS_UNIT + 1];

  void SetUp() override { w.begin(); }

  // Send one frame through the receive path. Report the value that recvRaw()
  // returned.
  int receive(const char* payload, int16_t result) {
    radio.deliver((const uint8_t*)payload, strlen(payload), result);
    return w.recvRaw(buf, MAX_TRANS_UNIT);
  }
};

TEST_F(RecvErrors, AGoodFrameCountsAsReceivedAndNothingElse) {
  EXPECT_EQ(receive("hello", RADIOLIB_ERR_NONE), 5);
  EXPECT_EQ(w.getPacketsRecv(), 1u);
  EXPECT_EQ(w.getPacketsRecvErrors(), 0u);
  EXPECT_EQ(w.getRecvErrCrc(), 0u);
  EXPECT_EQ(w.getRecvErrOther(), 0u);
}

TEST_F(RecvErrors, EachCauseIncrementsOnlyItsOwnCounter) {
  struct { int16_t err; uint32_t (RadioLibWrapper::*counter)() const; } cases[] = {
    { RADIOLIB_ERR_CRC_MISMATCH,        &RadioLibWrapper::getRecvErrCrc },
    { RADIOLIB_ERR_LORA_HEADER_DAMAGED, &RadioLibWrapper::getRecvErrHeader },
    { RADIOLIB_ERR_RX_TIMEOUT,          &RadioLibWrapper::getRecvErrTimeout },
    { RADIOLIB_ERR_WRONG_MODEM,         &RadioLibWrapper::getRecvErrOther },
  };

  uint32_t n = 0;
  for (auto& c : cases) {
    w.resetStats();
    EXPECT_EQ(receive("damaged", c.err), 0) << "err=" << c.err;   // the code discards the frame
    EXPECT_EQ((w.*(c.counter))(), 1u) << "err=" << c.err;
    EXPECT_EQ(w.getPacketsRecvErrors(), 1u) << "err=" << c.err;
    EXPECT_EQ(w.getPacketsRecv(), 0u) << "err=" << c.err;
    n += w.getRecvErrCrc() + w.getRecvErrHeader() + w.getRecvErrTimeout() + w.getRecvErrOther();
  }
  EXPECT_EQ(n, 4u);   // one counter each, never two
}

TEST_F(RecvErrors, TheCausesSumToTheErrorTotal) {
  receive("a", RADIOLIB_ERR_CRC_MISMATCH);
  receive("b", RADIOLIB_ERR_CRC_MISMATCH);
  receive("c", RADIOLIB_ERR_RX_TIMEOUT);
  receive("d", RADIOLIB_ERR_NONE);

  EXPECT_EQ(w.getRecvErrCrc(), 2u);
  EXPECT_EQ(w.getRecvErrTimeout(), 1u);
  EXPECT_EQ(w.getPacketsRecvErrors(), 3u);
  EXPECT_EQ(w.getRecvErrCrc() + w.getRecvErrHeader() + w.getRecvErrTimeout() + w.getRecvErrOther(),
            w.getPacketsRecvErrors());
}

TEST_F(RecvErrors, TheLastErrorIsTheRadioCodeNotABucket) {
  receive("a", RADIOLIB_ERR_CRC_MISMATCH);
  receive("b", RADIOLIB_ERR_WRONG_MODEM);
  EXPECT_EQ(w.getLastRecvError(), RADIOLIB_ERR_WRONG_MODEM);
}

TEST_F(RecvErrors, ResetStatsClearsEveryCause) {
  receive("a", RADIOLIB_ERR_CRC_MISMATCH);
  receive("b", RADIOLIB_ERR_RX_TIMEOUT);
  w.resetStats();
  EXPECT_EQ(w.getRecvErrCrc(), 0u);
  EXPECT_EQ(w.getRecvErrTimeout(), 0u);
  EXPECT_EQ(w.getLastRecvError(), 0);
  EXPECT_EQ(w.getLastRecvErrorLen(), 0);
}

// This is the reason for the capture. The mesh side loses the bytes. But the
// radio read them out before it found that the frame was bad.
TEST_F(RecvErrors, ACrcFailedFrameIsKept) {
  EXPECT_EQ(receive("corrupt", RADIOLIB_ERR_CRC_MISMATCH), 0);
  ASSERT_EQ(w.getLastRecvErrorLen(), 7);
  EXPECT_EQ(0, memcmp(w.getLastRecvErrorPayload(), "corrupt", 7));
}

// A timeout returns before RadioLib reads the FIFO. If the code kept the
// buffer, it would keep the previous frame and call it evidence.
TEST_F(RecvErrors, ATimeoutLeavesNoPayloadBehind) {
  receive("corrupt", RADIOLIB_ERR_CRC_MISMATCH);
  receive("stale bytes", RADIOLIB_ERR_RX_TIMEOUT);
  EXPECT_EQ(w.getLastRecvErrorLen(), 0);
}

TEST_F(RecvErrors, TheCaptureIsTruncatedToTheConfiguredSize) {
  uint8_t big[MAX_TRANS_UNIT];
  memset(big, 0x5A, sizeof(big));
  radio.deliver(big, sizeof(big), RADIOLIB_ERR_CRC_MISMATCH);
  w.recvRaw(buf, MAX_TRANS_UNIT);
  EXPECT_EQ(w.getLastRecvErrorLen(), RX_ERR_PAYLOAD_BYTES);
}

// A radio that cannot calculate a new price must still answer. The only true
// answer is our own value.
TEST(AirtimeAtCRDefault, TheBaseRadioPricesEverythingAtOurOwnRate) {
  PhysicalLayer radio;
  TestBoard board;
  TestWrapper w(radio, board);
  EXPECT_EQ(w.getEstAirtimeForCR(50, 8), w.getEstAirtimeFor(50));
}

class AirtimeAtCR : public ::testing::Test {
protected:
  PhysicalLayer radio;   // the default values: SF8, 62.5 kHz, 4/5, 32-symbol preamble, explicit, CRC
  TestBoard board;
  TestWrapper w{radio, board};

  uint32_t at(int len, uint8_t cr) { return w.estAirtimeAtCRHere(len, cr, radio); }
};

// A new price at the rate that we already use must not change the number. If it
// did, the two paths through checkRecv() would disagree about the same packet.
TEST_F(AirtimeAtCR, OurOwnCodingRateReproducesTheDriversFigure) {
  for (int len : {1, 16, 50, 100, 255}) {
    EXPECT_EQ(at(len, radio.cfg_cr), w.getEstAirtimeFor(len)) << "len=" << len;
  }
}

// 0 means "the radio could not tell us". The cause is an implicit header, a
// damaged header, or a radio with no register for the value. A guess is worse
// than our own value.
TEST_F(AirtimeAtCR, AnUnknownCodingRateFallsBackToOurs) {
  EXPECT_EQ(at(50, 0), w.getEstAirtimeFor(50));
  for (uint8_t cr : {1, 2, 3, 4, 9, 255}) {
    EXPECT_EQ(at(50, cr), w.getEstAirtimeFor(50)) << "cr=" << (int)cr;
  }
}

// These values come from the datasheet, calculated by hand at SF8, 62.5 kHz,
// 4/5, a 32-symbol preamble, an explicit header, CRC on, and LDRO off.
// T_sym = 4.096 ms, and
// n_payload = 8 + ceil((8*len - 4*SF + 28 + 16)/(4*SF)) * 5, so
//   50 B  -> 73 payload symbols,  109.25 total -> 447.488 ms
//   100 B -> 138 payload symbols, 174.25 total -> 713.728 ms
//   255 B -> 333 payload symbols, 369.25 total -> 1512.448 ms
// The wrapper reports whole milliseconds and cuts the fraction. Thus 100 B
// gives 713.
TEST_F(AirtimeAtCR, KnownValuesAtTheStandardPreset) {
  EXPECT_EQ(at(50, 5), 447u);
  EXPECT_EQ(at(100, 5), 713u);
  EXPECT_EQ(at(255, 5), 1512u);
}

// This is the reason for the work. A neighbour at 4/8 held the channel longer
// than the same bytes cost us at 4/5.
TEST_F(AirtimeAtCR, AHeavierCodeCostsMoreAirtime) {
  EXPECT_LT(at(100, 5), at(100, 6));
  EXPECT_LT(at(100, 6), at(100, 7));
  EXPECT_LT(at(100, 7), at(100, 8));
  EXPECT_EQ(at(100, 8), 1033u);   // 26 coded symbols at 4/8 rather than 4/5
}

// The coding rate affects only the payload symbols. The preamble and the fixed
// sync symbols keep the same length for every choice of the sender. Thus a
// longer preamble must move every coding rate by exactly the same amount.
TEST_F(AirtimeAtCR, OnlyThePayloadSymbolsAreCoded) {
  uint32_t cr5_short = at(100, 5), cr8_short = at(100, 8);
  radio.cfg_preamble = 64;   // 32 more symbols, so 131.072 ms more, at any CR
  EXPECT_EQ(at(100, 5), cr5_short + 131u);
  EXPECT_EQ(at(100, 8), cr8_short + 131u);
  EXPECT_EQ(at(100, 8) - at(100, 5), cr8_short - cr5_short);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
