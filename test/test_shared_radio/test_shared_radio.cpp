// SharedRadioCore: the arbiter that lets several mesh identities share one
// half-duplex LoRa radio. These tests cover the behaviours that were found the
// hard way on hardware — RX fan-out accounting, TX serialisation, the on-board
// loopback that lets identities hear each other, and runtime port activation.

#include <gtest/gtest.h>
#include "helpers/SharedRadio.h"

unsigned long g_fake_millis = 0;
FakeSerial Serial;

namespace {

// A scriptable stand-in for the physical radio.
class FakeRadio : public mesh::Radio {
public:
  std::vector<std::vector<uint8_t>> sent;
  std::vector<uint8_t> pending_rx;      // next frame recvRaw() will hand out
  bool send_ok = true;
  bool send_complete = true;
  int  begin_calls = 0;
  int  finish_calls = 0;
  uint32_t recv_errors = 0;

  void begin() override { begin_calls++; }
  int recvRaw(uint8_t* bytes, int sz) override {
    if (pending_rx.empty()) return 0;
    int n = (int)pending_rx.size(); if (n > sz) n = sz;
    memcpy(bytes, pending_rx.data(), n);
    pending_rx.clear();
    return n;
  }
  bool startSendRaw(const uint8_t* bytes, int len) override {
    if (!send_ok) return false;
    sent.emplace_back(bytes, bytes + len);
    return true;
  }
  bool isSendComplete() override { return send_complete; }
  void onSendFinished() override { finish_calls++; }
  float getLastSNR() const override { return 5.5f; }
  float getLastRSSI() const override { return -88; }
};

struct Fixture {
  FakeRadio radio;
  SharedRadioCore core{radio};
  RadioPort a, b, c;
  int ia, ib, ic;
  Fixture(bool loopback = false) {
    ia = core.addPort(a); ib = core.addPort(b); ic = core.addPort(c);
    core.setLoopback(loopback);
  }
  void deliver(std::vector<uint8_t> frame) { radio.pending_rx = std::move(frame); core.pump(); }
};

int take(RadioPort& p, uint8_t* buf) { return p.recvRaw(buf, MAX_TRANS_UNIT); }

// ---------------------------------------------------------------- RX fan-out

TEST(SharedRadio, EveryPortReceivesTheFrameExactlyOnce) {
  Fixture f;
  f.deliver({0x11, 0x22, 0x33});
  uint8_t buf[MAX_TRANS_UNIT];

  EXPECT_EQ(3, take(f.a, buf));
  EXPECT_EQ(0x11, buf[0]);
  EXPECT_EQ(3, take(f.b, buf));
  EXPECT_EQ(3, take(f.c, buf));
  // second read by the same port yields nothing
  EXPECT_EQ(0, take(f.a, buf));
}

TEST(SharedRadio, FrameIsHeldUntilEveryPortHasConsumedIt) {
  Fixture f;
  f.deliver({0xAA});
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));

  // a new frame arrives at the radio, but pump() must not fetch it yet
  f.radio.pending_rx = {0xBB};
  f.core.pump();
  ASSERT_EQ(1, take(f.b, buf));
  EXPECT_EQ(0xAA, buf[0]) << "port b must still see the first frame";

  ASSERT_EQ(1, take(f.c, buf));
  f.core.pump();                      // now the next frame may be fetched
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(0xBB, buf[0]);
}

TEST(SharedRadio, PortMetadataMatchesTheReceivedFrame) {
  Fixture f;
  f.deliver({0x01});
  uint8_t buf[MAX_TRANS_UNIT];
  take(f.a, buf);
  EXPECT_FLOAT_EQ(5.5f, f.a.getLastSNR());
  EXPECT_FLOAT_EQ(-88.0f, f.a.getLastRSSI());
}

// ------------------------------------------------------------ TX serialising

TEST(SharedRadio, OnlyOnePortMayTransmitAtATime) {
  Fixture f;
  uint8_t msg[] = {1, 2, 3};
  f.radio.send_complete = false;                 // transmit still in flight

  EXPECT_TRUE(f.a.startSendRaw(msg, 3));
  EXPECT_FALSE(f.b.startSendRaw(msg, 3)) << "sibling must be refused while the transmitter is held";
  EXPECT_TRUE(f.b.isReceiving()) << "siblings should read the channel as busy so they back off";

  f.a.onSendFinished();
  EXPECT_TRUE(f.b.startSendRaw(msg, 3)) << "transmitter must be released";
}

TEST(SharedRadio, PumpLeavesTheRadioAloneWhileTransmitting) {
  Fixture f;
  uint8_t msg[] = {9};
  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));

  f.radio.pending_rx = {0x77};
  f.core.pump();                                  // must NOT re-arm RX mid-transmit
  EXPECT_FALSE(f.radio.pending_rx.empty()) << "pump() must not touch the radio during a send";
}

TEST(SharedRadio, RealRadioBeginRunsExactlyOnceAcrossPorts) {
  Fixture f;
  f.a.begin(); f.b.begin(); f.c.begin();
  EXPECT_EQ(1, f.radio.begin_calls) << "the DIO1 ISR must be attached once, and at least once";
}

// ------------------------------------------------------------------ loopback

TEST(SharedRadioLoopback, SiblingsHearALocalTransmitButTheSenderDoesNot) {
  Fixture f(/*loopback=*/true);
  uint8_t msg[] = {0x42, 0x43};
  ASSERT_TRUE(f.a.startSendRaw(msg, 2));
  f.a.onSendFinished();
  f.core.pump();                                   // drains the loopback queue

  uint8_t buf[MAX_TRANS_UNIT];
  EXPECT_EQ(0, take(f.a, buf)) << "a node must not hear its own transmission";
  ASSERT_EQ(2, take(f.b, buf));
  EXPECT_EQ(0x42, buf[0]);
  EXPECT_EQ(2, take(f.c, buf));
}

TEST(SharedRadioLoopback, DisabledMeansIdentitiesStayDeafToEachOther) {
  Fixture f(/*loopback=*/false);
  uint8_t msg[] = {0x42};
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  f.a.onSendFinished();
  f.core.pump();

  uint8_t buf[MAX_TRANS_UNIT];
  EXPECT_EQ(0, take(f.b, buf));
}

TEST(SharedRadioLoopback, LoopbackFrameCarriesAStrongSyntheticSignal) {
  Fixture f(true);
  uint8_t msg[] = {1};
  f.a.startSendRaw(msg, 1);
  f.a.onSendFinished();
  f.core.pump();
  uint8_t buf[MAX_TRANS_UNIT];
  take(f.b, buf);
  EXPECT_GT(f.b.getLastSNR(), 0) << "same-board link should look excellent, not marginal";
}

// --------------------------------------------------- runtime port activation

TEST(SharedRadioPorts, InactivePortNeitherReceivesNorStallsDelivery) {
  Fixture f;
  f.core.setPortActive(f.ic, false);
  f.deliver({0x55});
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));
  ASSERT_EQ(1, take(f.b, buf));
  EXPECT_EQ(0, take(f.c, buf)) << "a silenced identity must not receive";

  // with a and b done, the frame is fully consumed even though c never took it
  f.radio.pending_rx = {0x66};
  f.core.pump();
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(0x66, buf[0]) << "pump() must not wait on an inactive port";
}

TEST(SharedRadioPorts, ActivatingMidFlightDoesNotStallTheCurrentFrame) {
  Fixture f;
  f.core.setPortActive(f.ic, false);
  f.deliver({0x01});
  uint8_t buf[MAX_TRANS_UNIT];
  take(f.a, buf); take(f.b, buf);

  f.core.setPortActive(f.ic, true);   // identity started while a frame is in flight
  f.radio.pending_rx = {0x02};
  f.core.pump();
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(0x02, buf[0]) << "the in-flight frame must not block on a just-activated port";
}

TEST(SharedRadioPorts, DeactivatingTheTransmitterOwnerReleasesTheRadio) {
  Fixture f;
  uint8_t msg[] = {7};
  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));

  f.core.setPortActive(f.ia, false);   // identity stopped mid-transmit
  EXPECT_TRUE(f.b.startSendRaw(msg, 1)) << "stopping the owner must not strand the transmitter";
}

TEST(SharedRadioPorts, ActivationStateIsReported) {
  Fixture f;
  EXPECT_TRUE(f.core.portActive(f.ia));
  f.core.setPortActive(f.ia, false);
  EXPECT_FALSE(f.core.portActive(f.ia));
}

// ---------------------------------------------------------------- packet log

TEST(SharedRadioTrace, RecordsReceivesAndTransmitsWithDirection) {
  Fixture f;
  f.deliver({0x10, 0x20});
  uint8_t msg[] = {0x30};
  f.a.startSendRaw(msg, 1);

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(2, n);
  EXPECT_EQ(-1, entries[0].dir) << "received frames are logged with dir -1";
  EXPECT_EQ(0x10, entries[0].hdr);
  EXPECT_EQ(f.ia, entries[1].dir) << "transmits are attributed to the sending port";
  EXPECT_EQ(PKT_FLAG_OK, entries[1].flag);
  EXPECT_EQ(1u, f.core.rxTotal());
  EXPECT_EQ(1u, f.core.txTotal());
}

TEST(SharedRadioTrace, FailedTransmitIsLoggedAndExcludedFromTotals) {
  Fixture f;
  uint8_t msg[] = {1};
  f.radio.send_complete = false;          // TX-done never arrives
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  f.a.isSendComplete();
  f.a.onSendFinished();                    // dispatcher gives up

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(2, n);
  EXPECT_EQ(PKT_FLAG_TX_FAIL, entries[1].flag);
  EXPECT_EQ(1u, f.core.txTotal()) << "the failure must not inflate the transmit count";
}

TEST(SharedRadioTrace, OnlyReturnsEntriesNewerThanTheCallersCursor) {
  Fixture f;
  uint8_t buf[MAX_TRANS_UNIT];
  f.deliver({1});
  take(f.a, buf); take(f.b, buf); take(f.c, buf);   // frame must be fully consumed
  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n1 = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(1, n1);
  uint32_t cursor = entries[0].seq;

  f.deliver({2});
  int n2 = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, cursor);
  ASSERT_EQ(1, n2);
  EXPECT_EQ(2, entries[0].hdr);
}

}  // namespace

// --------------------------------------------- corrupt frames are preserved

// The driver reports CRC failures only through a counter and drops the frame,
// but it has already read the damaged bytes. Those bytes are worth keeping: the
// same packet often arrives intact moments later in a burst, and comparing the
// two shows how much was actually hit.
namespace {
const uint8_t* g_bad_payload = nullptr;
uint8_t g_bad_len = 0;
uint32_t g_err_count = 0;
int16_t g_err_code = 0;
}

TEST(SharedRadioCorrupt, CrcFailureIsLoggedWithItsDamagedBytesAndErrorCode) {
  Fixture f;
  static const uint8_t damaged[] = {0x11, 0xDE, 0xAD, 0xBE, 0xEF};
  g_bad_payload = damaged; g_bad_len = sizeof(damaged);
  g_err_count = 0; g_err_code = -7;      // RADIOLIB_ERR_CRC_MISMATCH
  f.core.setRxErrorCounter([]() -> uint32_t { return g_err_count; },
                           []() -> int16_t { return g_err_code; },
                           []() -> const uint8_t* { return g_bad_payload; },
                           []() -> uint8_t { return g_bad_len; });

  g_err_count = 1;                        // the driver saw a CRC failure
  f.core.pump();

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(1, n);
  EXPECT_EQ(PKT_FLAG_RX_ERR, entries[0].flag);
  EXPECT_EQ(-7, entries[0].aux) << "the cause (CRC vs header damage) must survive";
  ASSERT_EQ(sizeof(damaged), entries[0].raw_len) << "damaged bytes must be kept for decoding";
  EXPECT_EQ(0, memcmp(damaged, entries[0].raw, sizeof(damaged)));
  EXPECT_EQ(0x11, entries[0].hdr);
  EXPECT_EQ(0u, f.core.rxTotal()) << "a corrupt frame is not a received packet";
}

TEST(SharedRadioCorrupt, ErrorWithNoRecoverableBytesStillLogsTheEvent) {
  Fixture f;
  g_bad_payload = nullptr; g_bad_len = 0;
  g_err_count = 0; g_err_code = -16;      // header damaged: nothing readable
  f.core.setRxErrorCounter([]() -> uint32_t { return g_err_count; },
                           []() -> int16_t { return g_err_code; },
                           []() -> const uint8_t* { return g_bad_payload; },
                           []() -> uint8_t { return g_bad_len; });
  g_err_count = 1;
  f.core.pump();

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  ASSERT_EQ(1, f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0));
  EXPECT_EQ(PKT_FLAG_RX_ERR, entries[0].flag);
  EXPECT_EQ(0, entries[0].raw_len);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
