#include <gtest/gtest.h>

#include "helpers/radiolib/RadioLibWrappers.h"

// Minimal concrete wrapper: RadioLibWrapper leaves three hooks abstract, and
// none of them are involved in what these tests cover.
class TestWrapper : public RadioLibWrapper {
public:
  TestWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) {}
  bool isReceivingPacket() override { return false; }
  float getCurrentRSSI() override { return -120.0f; }
  void setParams(float, float, uint8_t, uint8_t) override {}

  // What CustomSX1262Wrapper::getEstAirtimeForCR() does, minus the godmode
  // field reads that only a real SX126x has: the modem settings come from the
  // stand-in radio instead, so the shared helper is what is under test.
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

// 0 is "no frame decoded yet"; 5..7 are reserved in the LoRa header. Reporting
// any of them as a coding rate would be a guess, and callers cannot tell a
// guess from a reading.
TEST(CodingRateDecode, EverythingElseIsUnknown) {
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(0), 0);
  for (int raw = 5; raw <= 255; raw++) {
    EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate((uint8_t)raw), 0) << "raw=" << raw;
  }
}

// A radio that cannot read the field must say so rather than assume our own
// coding rate: in explicit-header mode the value belongs to the sender.
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

  // Push one frame through the receive path and report what recvRaw() returned.
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
    EXPECT_EQ(receive("damaged", c.err), 0) << "err=" << c.err;   // frame is discarded
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

// The point of the capture: the bytes are gone from the mesh's side, but the
// radio had already read them out before deciding the frame was bad.
TEST_F(RecvErrors, ACrcFailedFrameIsKept) {
  EXPECT_EQ(receive("corrupt", RADIOLIB_ERR_CRC_MISMATCH), 0);
  ASSERT_EQ(w.getLastRecvErrorLen(), 7);
  EXPECT_EQ(0, memcmp(w.getLastRecvErrorPayload(), "corrupt", 7));
}

// A timeout returns before RadioLib reads the FIFO, so keeping the buffer would
// be keeping the previous frame and calling it evidence.
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

// A radio that cannot reprice must still answer, and the only honest answer is
// our own figure.
TEST(AirtimeAtCRDefault, TheBaseRadioPricesEverythingAtOurOwnRate) {
  PhysicalLayer radio;
  TestBoard board;
  TestWrapper w(radio, board);
  EXPECT_EQ(w.getEstAirtimeForCR(50, 8), w.getEstAirtimeFor(50));
}

class AirtimeAtCR : public ::testing::Test {
protected:
  PhysicalLayer radio;   // defaults: SF8, 62.5 kHz, 4/5, 32-symbol preamble, explicit, CRC
  TestBoard board;
  TestWrapper w{radio, board};

  uint32_t at(int len, uint8_t cr) { return w.estAirtimeAtCRHere(len, cr, radio); }
};

// Repricing at the rate we are already using must not move the number, or the
// two paths through checkRecv() would disagree about the same packet.
TEST_F(AirtimeAtCR, OurOwnCodingRateReproducesTheDriversFigure) {
  for (int len : {1, 16, 50, 100, 255}) {
    EXPECT_EQ(at(len, radio.cfg_cr), w.getEstAirtimeFor(len)) << "len=" << len;
  }
}

// 0 is "the radio could not tell us" -- implicit header, a damaged header, a
// radio with no register for it. Guessing would be worse than our own figure.
TEST_F(AirtimeAtCR, AnUnknownCodingRateFallsBackToOurs) {
  EXPECT_EQ(at(50, 0), w.getEstAirtimeFor(50));
  for (uint8_t cr : {1, 2, 3, 4, 9, 255}) {
    EXPECT_EQ(at(50, cr), w.getEstAirtimeFor(50)) << "cr=" << (int)cr;
  }
}

// Worked from the datasheet by hand at SF8 / 62.5 kHz / 4/5 / 32-symbol
// preamble / explicit header / CRC on / LDRO off. T_sym = 4.096 ms, and
// n_payload = 8 + ceil((8*len - 4*SF + 28 + 16)/(4*SF)) * 5, so
//   50 B  -> 73 payload symbols,  109.25 total -> 447.488 ms
//   100 B -> 138 payload symbols, 174.25 total -> 713.728 ms
//   255 B -> 333 payload symbols, 369.25 total -> 1512.448 ms
// The wrapper reports whole milliseconds and truncates, so 100 B is 713.
TEST_F(AirtimeAtCR, KnownValuesAtTheStandardPreset) {
  EXPECT_EQ(at(50, 5), 447u);
  EXPECT_EQ(at(100, 5), 713u);
  EXPECT_EQ(at(255, 5), 1512u);
}

// The whole point of the exercise: a neighbour on 4/8 held the channel longer
// than the same bytes would have cost us at 4/5.
TEST_F(AirtimeAtCR, AHeavierCodeCostsMoreAirtime) {
  EXPECT_LT(at(100, 5), at(100, 6));
  EXPECT_LT(at(100, 6), at(100, 7));
  EXPECT_LT(at(100, 7), at(100, 8));
  EXPECT_EQ(at(100, 8), 1033u);   // 26 coded symbols at 4/8 rather than 4/5
}

// Coding applies to the payload only; the preamble and the fixed sync symbols
// are the same length whatever the sender chose. So lengthening the preamble
// must shift every coding rate by exactly the same amount.
TEST_F(AirtimeAtCR, OnlyThePayloadSymbolsAreCoded) {
  uint32_t cr5_short = at(100, 5), cr8_short = at(100, 8);
  radio.cfg_preamble = 64;   // 32 more symbols, 131.072 ms more, at any CR
  EXPECT_EQ(at(100, 5), cr5_short + 131u);
  EXPECT_EQ(at(100, 8), cr8_short + 131u);
  EXPECT_EQ(at(100, 8) - at(100, 5), cr8_short - cr5_short);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
