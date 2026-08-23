// SharedRadioCore is the arbiter. It lets several mesh identities share one
// half-duplex LoRa radio. These tests cover the behaviour that the developers
// found on hardware with difficulty: the RX fan-out counts, the TX order, the
// on-board loopback that lets the identities hear each other, and the port
// activation at run time.

#include <gtest/gtest.h>
#include "helpers/SharedRadio.h"

unsigned long g_fake_millis = 0;
FakeSerial Serial;

namespace {

// This class replaces the physical radio. The test controls it.
class FakeRadio : public mesh::Radio {
public:
  std::vector<std::vector<uint8_t>> sent;
  std::vector<uint8_t> pending_rx;      // the next frame that recvRaw() gives out
  bool send_ok = true;
  bool send_complete = true;
  int  begin_calls = 0;
  int  finish_calls = 0;
  uint32_t recv_errors = 0;
  bool cad = false;
  int  threshold = 0;
  int  calib_calls = 0;

  void begin() override { begin_calls++; }
  void setCADEnabled(bool on) override { cad = on; }
  void triggerNoiseFloorCalibrate(int t) override { threshold = t; calib_calls++; }
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
  bool receiving = false;
  bool isReceiving() override { return receiving; }
  uint8_t rx_cr = 0;                  // the coding rate of the frame that recvRaw() gave out
  uint8_t getLastRxCodingRate() const override { return rx_cr; }
  float getLastSNR() const override { return 5.5f; }
  float getLastRSSI() const override { return -88; }
  uint32_t getEstAirtimeFor(int len) override { return (uint32_t)len; }
  uint32_t getEstAirtimeForCR(int len, uint8_t cr) override {
    return cr ? (uint32_t)len * cr : getEstAirtimeFor(len);   // 1 ms per byte for each 4/x step
  }
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
  // a second read by the same port gives nothing
  EXPECT_EQ(0, take(f.a, buf));
}

TEST(SharedRadio, ASlowPortStillReceivesEveryFrameInOrder) {
  Fixture f;
  f.deliver({0xAA});
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));

  // A second frame arrives while b and c are still behind. The core must take it
  // off the radio immediately. A wait for b and c loses packets. But b and c
  // must still get the frames in the order of arrival.
  f.deliver({0xBB});
  ASSERT_EQ(1, take(f.b, buf));
  EXPECT_EQ(0xAA, buf[0]) << "port b must still see the first frame";
  ASSERT_EQ(1, take(f.b, buf));
  EXPECT_EQ(0xBB, buf[0]);

  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(0xBB, buf[0]);
  ASSERT_EQ(1, take(f.c, buf));
  EXPECT_EQ(0xAA, buf[0]);
  ASSERT_EQ(1, take(f.c, buf));
  EXPECT_EQ(0xBB, buf[0]);
  EXPECT_EQ(0u, f.core.rxDropped());
}

// This is the reason for the queue. The buffer of the radio holds ONE packet.
// The core loses any packet that it does not read before the next one arrives.
// The core must never wait for the identities before it empties the buffer.
TEST(SharedRadio, TheRadioIsDrainedEvenWhenNoPortIsReading) {
  Fixture f;
  for (uint8_t i = 0; i < 4; i++) f.deliver({(uint8_t)(0xA0 + i)});
  EXPECT_EQ(4, f.core.rxQueued());
  EXPECT_EQ(0u, f.core.rxDropped());

  uint8_t buf[MAX_TRANS_UNIT];
  for (uint8_t i = 0; i < 4; i++) {
    ASSERT_EQ(1, take(f.a, buf));
    EXPECT_EQ(0xA0 + i, buf[0]) << "frames must arrive in the order they were heard";
  }
  EXPECT_EQ(0, take(f.a, buf));
}

TEST(SharedRadio, AFullQueueDiscardsTheOldestFrameAndCountsIt) {
  Fixture f;
  const int slots = SharedRadioCore::RX_SLOTS, over = 3;
  for (int i = 0; i < slots + over; i++) f.deliver({(uint8_t)i});
  EXPECT_EQ(slots, f.core.rxQueued());
  EXPECT_EQ((uint32_t)over, f.core.rxDropped())
      << "overflow must be visible, not silent";

  // The NEWEST frames stay, because they still move through the mesh.
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(over, buf[0]);
}

// A frame goes away as soon as every port that listens has taken it. Thus a
// steady stream never fills the queue.
TEST(SharedRadio, FullyConsumedFramesFreeTheirSlots) {
  Fixture f;
  uint8_t buf[MAX_TRANS_UNIT];
  const int rounds = SharedRadioCore::RX_SLOTS * 3;
  for (int i = 0; i < rounds; i++) {
    f.deliver({(uint8_t)i});
    take(f.a, buf); take(f.b, buf); take(f.c, buf);
    ASSERT_EQ(0, f.core.rxQueued());
  }
  EXPECT_EQ(0u, f.core.rxDropped());
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
  f.radio.send_complete = false;                 // the transmit is still in progress

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
  f.core.pump();                                  // the core must NOT start RX again here
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
  f.core.pump();                                   // this empties the loopback queue

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

  // a and b are finished, so the frame is complete. c never took it.
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

  f.core.setPortActive(f.ic, true);   // the identity starts while a frame is in progress
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

  f.core.setPortActive(f.ia, false);   // the identity stops during a transmit
  EXPECT_TRUE(f.b.startSendRaw(msg, 1)) << "stopping the owner must not strand the transmitter";
}

TEST(SharedRadioPorts, ActivationStateIsReported) {
  Fixture f;
  EXPECT_TRUE(f.core.portActive(f.ia));
  f.core.setPortActive(f.ia, false);
  EXPECT_FALSE(f.core.portActive(f.ia));
}

// ------------------------------------------------- shared collision avoidance
// The dispatcher of each identity sends its own CAD setting and its own
// interference threshold to the radio every two seconds. The board has only one
// radio. Thus the core must combine the settings. It must not let the last
// writer win.

TEST(SharedRadioPolicy, CADStaysOnIfAnyIdentityWantsIt) {
  Fixture f;
  f.a.setCADEnabled(true);
  EXPECT_TRUE(f.radio.cad);
  f.b.setCADEnabled(false);            // the setting of b must not disable CAD for a
  EXPECT_TRUE(f.radio.cad);
  f.a.setCADEnabled(false);            // now no identity wants it
  EXPECT_FALSE(f.radio.cad);
}

TEST(SharedRadioPolicy, TheMostCautiousThresholdWins) {
  Fixture f;
  f.a.triggerNoiseFloorCalibrate(12);
  EXPECT_EQ(12, f.radio.threshold);
  f.b.triggerNoiseFloorCalibrate(6);   // this yields to weaker signals, so it is more careful
  EXPECT_EQ(6, f.radio.threshold);
  f.c.triggerNoiseFloorCalibrate(0);   // "off" must not switch the check off
  EXPECT_EQ(6, f.radio.threshold);
}

TEST(SharedRadioPolicy, ASilencedIdentityHasNoSay) {
  Fixture f;
  f.a.setCADEnabled(true);
  f.a.triggerNoiseFloorCalibrate(6);
  f.b.triggerNoiseFloorCalibrate(20);
  ASSERT_TRUE(f.radio.cad);
  ASSERT_EQ(6, f.radio.threshold);

  f.core.setPortActive(f.ia, false);
  EXPECT_FALSE(f.radio.cad) << "the only identity wanting CAD is no longer listening";
  f.b.triggerNoiseFloorCalibrate(20);
  EXPECT_EQ(20, f.radio.threshold);
}

TEST(SharedRadioPolicy, CalibrationKeepsTheStockCadenceNotThreeTimesIt) {
  Fixture f;
  g_fake_millis = 100000;
  f.a.triggerNoiseFloorCalibrate(6);
  int after_first = f.radio.calib_calls;

  // All three dispatchers ask inside the same window. The core calibrates once.
  f.b.triggerNoiseFloorCalibrate(6);
  f.c.triggerNoiseFloorCalibrate(6);
  EXPECT_EQ(after_first, f.radio.calib_calls);

  g_fake_millis += 2000;
  f.a.triggerNoiseFloorCalibrate(6);
  EXPECT_EQ(after_first + 1, f.radio.calib_calls);
}

TEST(SharedRadioPolicy, AChangedThresholdAppliesImmediately) {
  Fixture f;
  g_fake_millis = 200000;
  f.a.triggerNoiseFloorCalibrate(12);
  int n = f.radio.calib_calls;
  f.b.triggerNoiseFloorCalibrate(4);   // inside the rate limit, but the new value is important
  EXPECT_EQ(n + 1, f.radio.calib_calls);
  EXPECT_EQ(4, f.radio.threshold);
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
  f.radio.send_complete = false;          // the TX-done signal never arrives
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  f.a.isSendComplete();
  f.a.onSendFinished();                    // the dispatcher gives up

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(2, n);
  EXPECT_EQ(PKT_FLAG_TX_FAIL, entries[1].flag);
  EXPECT_EQ(1u, f.core.txTotal()) << "the failure must not inflate the transmit count";
}

// The coding rate is not one number. We know the rate that we transmit at. The
// sender owns the rate that we receive at, and puts it in the LoRa header. If
// the code joins the two, our own setting hides the important case. That case
// is a neighbour with a different preset.

// The Dispatcher of a port reads the coding rate from the packet that it just
// received. But the register of the modem now holds the rate of a later frame.
// Behind a fan-out queue, "the last frame decoded" and "the frame that this
// identity holds" are seldom the same frame.
TEST(SharedRadioCodingRate, EachPortReportsTheCodingRateOfTheFrameItTook) {
  Fixture f;
  uint8_t buf[MAX_TRANS_UNIT];

  f.radio.rx_cr = 8;
  f.deliver({0x11});
  EXPECT_EQ(1, take(f.a, buf));
  EXPECT_EQ(8, f.a.getLastRxCodingRate());

  f.radio.rx_cr = 6;              // a second sender, while b is still behind
  f.deliver({0x22});
  EXPECT_EQ(1, take(f.b, buf));
  EXPECT_EQ(8, f.b.getLastRxCodingRate()) << "b is holding the first frame, not the newest";
  EXPECT_EQ(1, take(f.b, buf));
  EXPECT_EQ(6, f.b.getLastRxCodingRate());
}

TEST(SharedRadioCodingRate, AnUnreadableCodingRateReachesThePortAsUnknown) {
  Fixture f;
  uint8_t buf[MAX_TRANS_UNIT];
  f.deliver({0x11});
  take(f.a, buf);
  EXPECT_EQ(0, f.a.getLastRxCodingRate());
}

// A looped-back frame never went on the air. Thus the only true coding rate for
// it is the rate that this node transmits at.
TEST(SharedRadioCodingRate, ALoopedBackFrameCarriesOurOwn) {
  Fixture f(true);
  f.core.setCodingRate(7);
  uint8_t msg[] = {0x30};
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  f.a.onSendFinished();
  f.core.pump();                 // this empties the loopback queue

  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.b, buf));
  EXPECT_EQ(7, f.b.getLastRxCodingRate());
}

// The port is itself a mesh::Radio. Thus the Dispatcher of a slot must price a
// receive through the port exactly as a single-identity node does.
TEST(SharedRadioCodingRate, APortPricesAirtimeAtTheRateItIsGiven) {
  Fixture f;
  EXPECT_EQ(32u, f.a.getEstAirtimeForCR(4, 8));
  EXPECT_EQ(f.a.getEstAirtimeFor(4), f.a.getEstAirtimeForCR(4, 0));
}

TEST(SharedRadioTrace, ReceivesCarryTheSendersCodingRateAndTransmitsOurs) {
  Fixture f;
  f.core.setCodingRate(5);
  f.radio.rx_cr = 8;                        // the sender uses 4/8 and we do not
  f.deliver({0x10, 0x20});
  uint8_t msg[] = {0x30};
  f.a.startSendRaw(msg, 1);

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(2, n);
  EXPECT_EQ(8, entries[0].cr) << "a receive is labelled with the CR its header carried";
  EXPECT_EQ(5, entries[1].cr) << "a transmit is labelled with the CR we send at";
}

// The channel-occupancy view is built on the airtime. Thus the code must price
// a receive at the CR that the sender used. FakeRadio charges 1 ms per byte for
// our own settings. The CR-aware function charges the CR of the sender instead.
TEST(SharedRadioTrace, ReceivedAirtimeIsPricedAtTheSendersCodingRate) {
  Fixture f;
  f.core.setCodingRate(5);
  f.radio.rx_cr = 8;
  f.deliver({1, 2, 3, 4});
  uint8_t msg[] = {1, 2, 3, 4};
  f.a.startSendRaw(msg, 4);

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(2, n);
  EXPECT_EQ(32, entries[0].air_ms) << "4 bytes at the sender's 4/8, not at our 4/5";
  EXPECT_EQ(4, entries[1].air_ms) << "our own transmit still comes from the driver";
}

TEST(SharedRadioTrace, CodingRateIsUnknownRatherThanGuessedAt) {
  Fixture f;
  f.core.setCodingRate(9);                 // not a denominator of 4/x, so the core refuses it
  EXPECT_EQ(0, f.core.codingRate());
  f.core.setCodingRate(7);
  f.radio.rx_cr = 0;                        // the radio could not report a coding rate
  f.deliver({0x10});

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(1, n);
  EXPECT_EQ(0, entries[0].cr) << "an unreadable receive CR must not fall back to our own";
}

TEST(SharedRadioTrace, OnlyReturnsEntriesNewerThanTheCallersCursor) {
  Fixture f;
  uint8_t buf[MAX_TRANS_UNIT];
  f.deliver({1});
  take(f.a, buf); take(f.b, buf); take(f.c, buf);   // every port must take the frame
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

// The driver reports a CRC failure only with a counter, and it discards the
// frame. But it has already read the damaged bytes. Those bytes are useful. The
// same packet often arrives complete a moment later in a burst. A comparison of
// the two packets shows how many bytes the noise hit.
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

  g_err_count = 1;                        // the driver found a CRC failure
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
  g_err_count = 0; g_err_code = -16;      // the header is damaged, so nothing is readable
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

// The CR is in the LoRa header. A CRC failure has a good header and a damaged
// payload, so the CR is still known. A header failure is different. The modem
// still holds the CR of the LAST frame that it decoded. To charge this packet
// with that CR would invent a fact. The radio here always answers 4/8. Only the
// error code decides whether that answer has a meaning.
TEST(SharedRadioCorrupt, ACrcFailureKeepsItsCodingRateButAHeaderFailureCannot) {
  static const uint8_t damaged[] = {0x11, 0xDE, 0xAD};
  auto trace_one = [](int16_t code) {
    Fixture f;
    g_bad_payload = damaged; g_bad_len = sizeof(damaged);
    g_err_count = 0; g_err_code = code;
    f.radio.rx_cr = 8;                 // the modem register holds an old value
    f.core.setRxErrorCounter([]() -> uint32_t { return g_err_count; },
                             []() -> int16_t { return g_err_code; },
                             []() -> const uint8_t* { return g_bad_payload; },
                             []() -> uint8_t { return g_bad_len; });
    g_err_count = 1;
    f.core.pump();
    PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
    EXPECT_EQ(1, f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0));
    return entries[0].cr;
  };
  EXPECT_EQ(8, trace_one(PKT_RX_ERR_CRC)) << "the header decoded, so its CR is real";
  EXPECT_EQ(0, trace_one(-16)) << "header damaged: the modem's CR belongs to another packet";
}

// ------------------------------------------------ stuck-transmitter watchdog

// pump() does not touch the radio during a transmit. This is deliberate. Thus a
// send that never completes takes the WHOLE BOARD off the air. Every identity
// goes deaf, not only the sender. This occurred in the field. A repeater advert
// held the transmitter for 3.4 hours, and the board received nothing in that
// time. The arbiter must take the radio back.

TEST(SharedRadioWatchdog, ATransmitThatNeverCompletesIsForceReleased) {
  Fixture f;
  g_fake_millis = 1000;
  uint8_t msg[] = {1, 2, 3};
  f.radio.send_complete = false;                 // the TX-done signal will never arrive
  ASSERT_TRUE(f.a.startSendRaw(msg, 3));

  // the core does not touch the radio during a normal transmit
  g_fake_millis += 2000;
  f.radio.pending_rx = {0xAA};
  f.core.pump();
  EXPECT_FALSE(f.radio.pending_rx.empty()) << "must not disturb a live transmit";
  EXPECT_EQ(0u, f.core.txStuck());

  // The core does not wait for ever.
  g_fake_millis += 20000;
  f.core.pump();
  EXPECT_EQ(1u, f.core.txStuck()) << "the watchdog must reclaim the transmitter";
  EXPECT_TRUE(f.radio.pending_rx.empty()) << "and the radio must be serviced again";
}

TEST(SharedRadioWatchdog, RecoveryRestoresReceiveAndLetsOthersTransmit) {
  Fixture f;
  g_fake_millis = 1000;
  uint8_t msg[] = {9};
  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  ASSERT_FALSE(f.b.startSendRaw(msg, 1)) << "blocked while the owner holds it";

  g_fake_millis += 20000;
  f.core.pump();                                  // the watchdog acts

  EXPECT_TRUE(f.b.startSendRaw(msg, 1)) << "another identity can transmit again";
  f.radio.send_complete = true;
  f.b.isSendComplete();
  f.b.onSendFinished();                           // let the send of b finish normally

  f.deliver({0x42});                              // and the receive path works
  uint8_t buf[MAX_TRANS_UNIT];
  EXPECT_EQ(1, take(f.b, buf));
}

TEST(SharedRadioWatchdog, ForcedReleaseIsLoggedWithHowLongItWasHeld) {
  Fixture f;
  g_fake_millis = 1000;
  uint8_t msg[] = {1};
  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  g_fake_millis += 30000;                         // held for 30 s
  f.core.pump();

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_GE(n, 2);
  const PktLogEntry& e = entries[n - 1];
  EXPECT_EQ(PKT_FLAG_TX_FAIL, e.flag);
  EXPECT_EQ(f.ia, e.dir) << "attributed to the identity that wedged the radio";
  EXPECT_GE(e.aux, 29) << "records the seconds it was held, for diagnosis";
}

TEST(SharedRadioWatchdog, NormalTransmitsAreNeverReclaimed) {
  Fixture f;
  g_fake_millis = 1000;
  uint8_t msg[] = {1};
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(f.a.startSendRaw(msg, 1));
    g_fake_millis += 700;                         // a realistic airtime
    ASSERT_TRUE(f.a.isSendComplete());
    f.a.onSendFinished();
    f.core.pump();
  }
  EXPECT_EQ(0u, f.core.txStuck()) << "the watchdog must not fire on healthy traffic";
}

// ---------------------------------------------------- radio health watchdog

// This is the real failure from the field. The transceiver stopped. Every
// RadioLib call then failed. The board was deaf for 3.4 hours and recorded no
// error anywhere. The radio refused the sends without a message, and the noise
// floor did not change.
namespace { int g_reinits = 0; }

TEST(SharedRadioHealth, ARefusedSendIsCountedAndTraced) {
  Fixture f;
  f.radio.send_ok = false;                      // the radio refuses every send
  uint8_t msg[] = {1, 2, 3};

  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  EXPECT_EQ(1u, f.core.txRefused()) << "a refused send is a transmit failure";
  EXPECT_EQ(0u, f.core.txTotal()) << "and must not count as a successful transmit";

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(1, n) << "it must leave evidence in the trace";
  EXPECT_EQ(PKT_FLAG_TX_FAIL, entries[0].flag);
  EXPECT_EQ(f.ia, entries[0].dir);
}

TEST(SharedRadioHealth, ASilentRadioIsReinitialised) {
  Fixture f;
  g_reinits = 0;
  g_fake_millis = 1000;
  f.core.setRadioReinit([]() { g_reinits++; });

  f.deliver({0x01});                            // a packet arrives, so the radio is alive
  uint8_t buf[MAX_TRANS_UNIT];
  take(f.a, buf); take(f.b, buf); take(f.c, buf);

  g_fake_millis += 300000;                      // 5 minutes of silence, which is normal
  f.core.pump();
  EXPECT_EQ(0, g_reinits) << "a quiet band must not trigger recovery";

  g_fake_millis += 700000;                      // now well past the limit
  f.core.pump();
  EXPECT_EQ(1, g_reinits) << "a radio silent this long is wedged, not quiet";
  EXPECT_EQ(1u, f.core.radioRecoveries());
}

TEST(SharedRadioHealth, RecoveryIsNotRetriedInATightLoop) {
  Fixture f;
  g_reinits = 0;
  g_fake_millis = 1000;
  f.core.setRadioReinit([]() { g_reinits++; });
  f.deliver({0x01});
  uint8_t buf[MAX_TRANS_UNIT];
  take(f.a, buf); take(f.b, buf); take(f.c, buf);

  g_fake_millis += 1000000;
  for (int i = 0; i < 50; i++) f.core.pump();
  EXPECT_EQ(1, g_reinits) << "one attempt, then wait again";
}

TEST(SharedRadioHealth, NoRecoveryBeforeTheRadioHasEverReceived) {
  Fixture f;
  g_reinits = 0;
  g_fake_millis = 1000;
  f.core.setRadioReinit([]() { g_reinits++; });
  g_fake_millis += 1000000;                     // a long start-up with no traffic yet
  f.core.pump();
  EXPECT_EQ(0, g_reinits) << "cannot judge a radio that has never heard anything";
}

// ------------------------------------------------------- relay confirmation

// This is the proof that a neighbour heard us. When a node relays a flood that
// we sent, it adds its own hash and transmits the flood again. Thus the packet
// that comes back past us still carries OUR hash in its path.
namespace {
// header byte: route=FLOOD(1), type=TXT_MSG(2) -> (2<<2)|1
const uint8_t FLOOD_HDR = (2 << 2) | 1;
// Build a flood frame. Its path holds the given hop hashes. The hash width is sz.
std::vector<uint8_t> floodWithPath(std::vector<std::vector<uint8_t>> hops, uint8_t sz) {
  std::vector<uint8_t> f{FLOOD_HDR};
  f.push_back((uint8_t)(((sz - 1) << 6) | hops.size()));
  for (auto& h : hops) for (uint8_t i = 0; i < sz; i++) f.push_back(h[i]);
  f.push_back(0xAA);   // payload
  return f;
}
const uint8_t SELF_KEY[32] = {0x30, 0x70, 0x30, 0x70};   // the first bytes of our public key
}

// A 1-byte hash collides once in every 256 packets. Thus "our" hash in a path
// at that width is not evidence that a node relayed us. The core counts it, but
// it must not confirm the transmit.
TEST(RelayConfirm, AOneByteMatchIsCountedButDoesNotConfirm) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);

  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  ASSERT_TRUE(f.a.startSendRaw(ours.data(), ours.size()));
  f.a.onSendFinished();
  EXPECT_EQ(1u, f.core.floodsSent(f.ia));

  g_fake_millis += 3000;
  f.deliver(floodWithPath({{0x30}}, 1));          // a 1-byte "match" of our hash
  EXPECT_EQ(0u, f.core.floodsConfirmed(f.ia)) << "1 byte is a coincidence, not proof";
  EXPECT_EQ(1u, f.core.confirmsByWidth(1)) << "still tallied for diagnostics";
  EXPECT_EQ(0u, f.core.confirmsByWidth(2));
}

TEST(RelayConfirm, TwoByteMatchesAreCountedSeparately) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  f.a.startSendRaw(ours.data(), ours.size());
  f.a.onSendFinished();

  g_fake_millis += 2000;
  f.deliver(floodWithPath({{0x30, 0x70}}, 2));
  EXPECT_EQ(1u, f.core.confirmsByWidth(2)) << "2-byte matches are the trustworthy ones";
  EXPECT_EQ(0u, f.core.confirmsByWidth(1));
}

TEST(RelayConfirm, AForeignPathDoesNotCount) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  f.a.startSendRaw(ours.data(), ours.size());
  f.a.onSendFinished();

  g_fake_millis += 2000;
  f.deliver(floodWithPath({{0x99}, {0xAB}}, 1));   // traffic from another node
  EXPECT_EQ(0u, f.core.floodsConfirmed(f.ia));
}

TEST(RelayConfirm, EachTransmitIsCreditedOnlyOnce) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  f.a.startSendRaw(ours.data(), ours.size());
  f.a.onSendFinished();

  uint8_t buf[MAX_TRANS_UNIT];
  for (int i = 0; i < 4; i++) {                    // four neighbours relay it
    g_fake_millis += 1000;
    f.deliver(floodWithPath({{0x30, 0x70}}, 2));   // 2 bytes, so this is a real confirmation
    take(f.a, buf); take(f.b, buf); take(f.c, buf);
  }
  EXPECT_EQ(1u, f.core.floodsConfirmed(f.ia))
      << "one transmit confirmed, however many nodes echo it";
}

TEST(RelayConfirm, AnOldTransmitIsNotCreditedByLateTraffic) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  f.a.startSendRaw(ours.data(), ours.size());
  f.a.onSendFinished();

  g_fake_millis += 120000;                         // long past the window
  f.deliver(floodWithPath({{0x30}}, 1));
  EXPECT_EQ(0u, f.core.floodsConfirmed(f.ia)) << "too late to be about our packet";
}

TEST(RelayConfirm, DirectSendsAreNotTrackedOnlyFloods) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  uint8_t direct[] = {(uint8_t)((2 << 2) | 2), 0x00, 0xAA};   // ROUTE_TYPE_DIRECT
  f.a.startSendRaw(direct, sizeof(direct));
  f.a.onSendFinished();
  EXPECT_EQ(0u, f.core.floodsSent(f.ia)) << "only floods get relayed onward";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ------------------------------------------------------------------ peer table
// Every forwarder adds its hash to the END of the path. Thus the position has a
// meaning. The last entry transmitted the frame that we received. The entry
// after one of our own entries received our transmission.

TEST(Peers, TheFinalHopIsANodeWeCanHear) {
  Fixture f;
  f.deliver(floodWithPath({{0xAA, 0x01}, {0xBB, 0x02}}, 2));

  ASSERT_EQ(2, f.core.numPeers());
  const auto* mid = f.core.peer(0);
  const auto* last = f.core.peer(1);
  EXPECT_EQ(0u, mid->direct_rx) << "a mid-path hop says nothing about its link to us";
  EXPECT_EQ(1u, mid->relays);
  EXPECT_EQ(1u, last->direct_rx) << "the final hop is the one whose radio we heard";
  EXPECT_EQ(1u, last->snr_n);
}

TEST(Peers, DistanceComesFromPositionFromTheEnd) {
  Fixture f;
  f.deliver(floodWithPath({{0xAA, 1}, {0xBB, 2}, {0xCC, 3}}, 2));
  ASSERT_EQ(3, f.core.numPeers());
  EXPECT_EQ(3, f.core.peer(0)->min_hops);
  EXPECT_EQ(2, f.core.peer(1)->min_hops);
  EXPECT_EQ(1, f.core.peer(2)->min_hops) << "the last forwarder is one hop away";
}

TEST(Peers, OnlyATwoByteMatchProvesTheyHeardUs) {
  Fixture f;
  f.core.setPortIdentity(f.ia, SELF_KEY);          // 0x30 0x70 ...

  // a 2-byte path: our hash, then their hash. They forwarded our transmission.
  f.deliver(floodWithPath({{0x30, 0x70}, {0xBB, 0x02}}, 2));
  const auto* p = f.core.peer(f.core.numPeers() - 1);
  EXPECT_EQ(1u, p->heard_us);
  EXPECT_EQ(0u, p->heard_us_1b);
  EXPECT_EQ(1, f.core.confirmedPeerCount());

  // the same path at 1 byte must NOT count as a confirmation
  Fixture g;
  g.core.setPortIdentity(g.ia, SELF_KEY);
  g.deliver(floodWithPath({{0x30}, {0xCC}}, 1));
  const auto* q = g.core.peer(g.core.numPeers() - 1);
  EXPECT_EQ(0u, q->heard_us) << "1 byte is 1-in-256, not proof";
  EXPECT_EQ(1u, q->heard_us_1b);
  EXPECT_EQ(0, g.core.confirmedPeerCount());
}

TEST(Peers, WeAreNotOurOwnPeer) {
  Fixture f;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  f.deliver(floodWithPath({{0x30, 0x70}, {0xBB, 0x02}}, 2));
  for (int i = 0; i < f.core.numPeers(); i++) {
    EXPECT_FALSE(f.core.peer(i)->hash[0] == 0x30 && f.core.peer(i)->hash[1] == 0x70);
  }
}

// The originator selects the hash width. Thus the same node arrives at 1 byte
// and at 2 bytes. A narrower sighting must join the one entry that it matches.
// It must not create a second entry for a node that does not exist.
TEST(Peers, ANarrowerSightingMergesIntoTheKnownNode) {
  Fixture f;
  f.deliver(floodWithPath({{0xAB, 0xCD}}, 2));
  ASSERT_EQ(1, f.core.numPeers());
  EXPECT_EQ(2, f.core.peer(0)->width);

  f.deliver(floodWithPath({{0xAB}}, 1));           // the same node, but a 1-byte path
  EXPECT_EQ(1, f.core.numPeers()) << "must not become a second peer";
  EXPECT_EQ(2u, f.core.peer(0)->direct_rx);
  EXPECT_EQ(2, f.core.peer(0)->width) << "the wider prefix is kept";
}

TEST(Peers, AWiderSightingRefinesAKnownPrefix) {
  Fixture f;
  f.deliver(floodWithPath({{0xAB}}, 1));
  ASSERT_EQ(1, f.core.numPeers());
  EXPECT_EQ(1, f.core.peer(0)->width);

  f.deliver(floodWithPath({{0xAB, 0xCD}}, 2));
  ASSERT_EQ(1, f.core.numPeers());
  EXPECT_EQ(2, f.core.peer(0)->width);
  EXPECT_EQ(0xCD, f.core.peer(0)->hash[1]);
}

// A 1-byte prefix can match two known nodes. Then nobody can tell which node
// sent it. The core must give it to neither node.
TEST(Peers, AnAmbiguousPrefixIsAttributedToNobody) {
  Fixture f;
  f.deliver(floodWithPath({{0xAB, 0x11}}, 2));
  f.deliver(floodWithPath({{0xAB, 0x22}}, 2));
  ASSERT_EQ(2, f.core.numPeers());
  uint32_t before0 = f.core.peer(0)->direct_rx, before1 = f.core.peer(1)->direct_rx;

  f.deliver(floodWithPath({{0xAB}}, 1));           // this matches both nodes
  EXPECT_EQ(2, f.core.numPeers()) << "must not invent a third node";
  EXPECT_EQ(before0, f.core.peer(0)->direct_rx);
  EXPECT_EQ(before1, f.core.peer(1)->direct_rx);
}

// ---------------------------------------------------------- advert identity
namespace {
const uint8_t ADVERT_HDR = (4 << 2) | 1;   // ADVERT, flood route
// [hdr][path_len][path][pub 32][ts 4][sig 64][flags][lat 4][lon 4][name]
std::vector<uint8_t> advert(std::vector<uint8_t> pub, const char* name,
                            int32_t lat_e6, int32_t lon_e6,
                            std::vector<std::vector<uint8_t>> hops = {}, uint8_t sz = 2) {
  std::vector<uint8_t> f{ADVERT_HDR};
  f.push_back((uint8_t)(((sz - 1) << 6) | hops.size()));
  for (auto& h : hops) for (uint8_t i = 0; i < sz; i++) f.push_back(h[i]);
  std::vector<uint8_t> p(100, 0);
  for (size_t i = 0; i < pub.size() && i < 32; i++) p[i] = pub[i];
  f.insert(f.end(), p.begin(), p.end());
  f.push_back(0x10 | 0x80);                                   // it has a position and a name
  for (int i = 0; i < 4; i++) f.push_back((lat_e6 >> (8*i)) & 0xFF);
  for (int i = 0; i < 4; i++) f.push_back((lon_e6 >> (8*i)) & 0xFF);
  for (const char* c = name; *c; c++) f.push_back((uint8_t)*c);
  return f;
}
}

TEST(PeerIdentity, ADirectAdvertRegistersANodeThatNeverForwards) {
  Fixture f;
  // the path is empty, so we received the transmission of the originator
  f.deliver(advert({0xDE, 0xAD, 0xBE}, "Leaf Node", -37762516, 144990310));

  ASSERT_EQ(1, f.core.numPeers());
  const auto* p = f.core.peer(0);
  EXPECT_EQ(1, p->min_hops) << "an unforwarded advert comes straight off its radio";
  EXPECT_EQ(1u, p->direct_rx);
  EXPECT_STREQ("Leaf Node", p->name);
  EXPECT_EQ(-37762516, p->lat_e6);
  EXPECT_EQ(144990310, p->lon_e6);
  EXPECT_EQ(0xDE, p->pub[0]);
}

TEST(PeerIdentity, AnAdvertNamesANodeAlreadyKnownOnlyAsAHash) {
  Fixture f;
  f.deliver(floodWithPath({{0xDE, 0xAD}}, 2));       // we saw it forward a frame, but it has no name
  ASSERT_EQ(1, f.core.numPeers());
  EXPECT_STREQ("", f.core.peer(0)->name);

  f.deliver(advert({0xDE, 0xAD, 0xBE}, "VIC-Preston", 1, 2));
  EXPECT_EQ(1, f.core.numPeers()) << "identity attaches to the existing peer";
  EXPECT_STREQ("VIC-Preston", f.core.peer(0)->name);
}

TEST(PeerIdentity, ARelayedAdvertPlacesTheOriginatorBeyondItsForwarders) {
  Fixture f;
  f.deliver(advert({0x11, 0x22, 0x33}, "Far", 0, 0, {{0xAA, 0x01}}, 2));
  // originator -> one forwarder -> us
  const SharedRadioCore::PeerEntry* orig = nullptr;
  for (int i = 0; i < f.core.numPeers(); i++)
    if (f.core.peer(i)->hash[0] == 0x11) orig = f.core.peer(i);
  ASSERT_NE(nullptr, orig);
  EXPECT_EQ(2, orig->min_hops);
  EXPECT_EQ(0u, orig->direct_rx) << "we heard the forwarder, not the originator";
}

TEST(PeerIdentity, DistantNodesDoNotConsumeSlots) {
  Fixture f;
  // three forwarders, so the originator is 4 hops away. That is past the near
  // limit.
  f.deliver(advert({0x99, 0x88, 0x77}, "Distant", 0, 0,
                   {{0xA1,1},{0xB2,2},{0xC3,3}}, 2));
  for (int i = 0; i < f.core.numPeers(); i++)
    EXPECT_NE(0x99, f.core.peer(i)->hash[0]) << "only near nodes earn an entry";
}

TEST(PeerIdentity, OurOwnAdvertIsIgnored) {
  Fixture f;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  f.deliver(advert({0x30, 0x70, 0x30, 0x70}, "Us", 1, 1));
  EXPECT_EQ(0, f.core.numPeers());
}

// ------------------------------------------------------- observer histograms
// The hop depth and the payload type are the cheapest useful data that the node
// can collect. They tell you whether the node sits among close neighbours or at
// the edge of a deep mesh. They also tell you which kind of traffic passes.

TEST(Observer, HopDepthOfReceivedTrafficIsBucketed) {
  Fixture f;
  f.deliver(floodWithPath({}, 2));                       // 0 hops: directly from a radio
  f.deliver(floodWithPath({{0xAA, 1}}, 2));              // 1 hop
  f.deliver(floodWithPath({{0xAA, 1}, {0xBB, 2}}, 2));   // 2 hops
  f.deliver(floodWithPath({{0xAA, 1}, {0xBB, 2}}, 2));   // 2 hops again

  const MeshObserver& o = f.core.observer();
  EXPECT_EQ(1u, o.hopCount(0));
  EXPECT_EQ(1u, o.hopCount(1));
  EXPECT_EQ(2u, o.hopCount(2));
  EXPECT_EQ(0u, o.hopCount(3));
}

TEST(Observer, PayloadTypesAreCounted) {
  Fixture f;
  f.deliver(floodWithPath({{0xAA, 1}}, 2));   // FLOOD_HDR is TXT_MSG (type 2)
  f.deliver(floodWithPath({{0xBB, 2}}, 2));
  f.deliver(advert({0xDE, 0xAD, 0xBE}, "N", 1, 2));   // ADVERT (type 4)

  const MeshObserver& o = f.core.observer();
  EXPECT_EQ(2u, o.typeCount(2));
  EXPECT_EQ(1u, o.typeCount(4));
  EXPECT_EQ(0u, o.typeCount(9));
  EXPECT_EQ(3u, o.framesObserved());
}

// The observer must work with no arbiter. That is the reason to move it out of
// the arbiter. A repeater with one identity can then use the same code.
TEST(Observer, WorksStandaloneWithoutAnyRadioOrPorts) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);

  std::vector<uint8_t> f = floodWithPath({{0x30, 0x70}, {0xCC, 0x01}}, 2);
  o.observeRx(f.data(), (int)f.size(), 20);

  ASSERT_EQ(1, o.numPeers()) << "our own hash must not become a peer";
  const auto* p = o.peer(0);
  EXPECT_EQ(0xCC, p->hash[0]);
  EXPECT_EQ(1u, p->heard_us) << "it relayed a packet carrying our hash";
  EXPECT_EQ(1u, p->direct_rx) << "and it was the final hop, so we heard it";
  EXPECT_EQ(1, o.confirmedPeerCount());
}

// The relay confirmation must also work with no arbiter. That is the reason for
// the move. A repeater with one identity needs it as much as the multi-identity
// board does.
TEST(Observer, RelayConfirmationStandalone) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  g_fake_millis = 5000;

  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  o.observeTx(ours.data(), (int)ours.size());
  EXPECT_EQ(1u, o.floodsSent());
  EXPECT_EQ(0u, o.floodsConfirmed());

  g_fake_millis += 2000;
  std::vector<uint8_t> back = floodWithPath({{0x30, 0x70}}, 2);
  o.observeRx(back.data(), (int)back.size(), 20);
  EXPECT_EQ(1u, o.floodsConfirmed()) << "our hash came back in a path";
  EXPECT_EQ(1u, o.confirmsByWidth(2));

  // no node can relay a direct send, so the core does not track it
  std::vector<uint8_t> direct{(uint8_t)((2 << 2) | 2), 0x00, 0xBB};
  o.observeTx(direct.data(), (int)direct.size());
  EXPECT_EQ(1u, o.floodsSent()) << "only floods are candidates";
}

// An echo has a meaning only if we transmitted a short time ago. Our hash is in
// the path of every packet that we forwarded. Thus a wide window gives the
// credit to the newest transmit, not to the transmit that came back.
TEST(Observer, ConfirmationWindowIsTight) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  EXPECT_EQ(5000u, o.confirmWindow()) << "default must cover one relay hop, not minutes";

  g_fake_millis = 10000;
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  o.observeTx(ours.data(), (int)ours.size());

  g_fake_millis += 4000;                                  // inside the window
  std::vector<uint8_t> back = floodWithPath({{0x30, 0x70}}, 2);
  o.observeRx(back.data(), (int)back.size(), 20);
  EXPECT_EQ(1u, o.floodsConfirmed()) << "4s after our send: plausibly ours";

  // a second send, then an echo that arrives long after it
  o.observeTx(ours.data(), (int)ours.size());
  g_fake_millis += 20000;                                 // well outside
  o.observeRx(back.data(), (int)back.size(), 20);
  EXPECT_EQ(1u, o.floodsConfirmed())
      << "20s later cannot be attributed to that transmit";
  // the core still counts it as a width observation, but gives it no credit
  EXPECT_EQ(2u, o.confirmsByWidth(2));
}

TEST(Observer, ConfirmationWindowIsTunable) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  o.setConfirmWindow(1000);                               // the short end of the 1 s to 5 s range
  g_fake_millis = 10000;
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  o.observeTx(ours.data(), (int)ours.size());

  g_fake_millis += 2500;
  std::vector<uint8_t> back = floodWithPath({{0x30, 0x70}}, 2);
  o.observeRx(back.data(), (int)back.size(), 20);
  EXPECT_EQ(0u, o.floodsConfirmed()) << "2.5s is outside a 1s window";
}

// ---- peer table eviction ---------------------------------------------------
// A full table used to stop completely. When every slot was full, the code
// could never record a new node again. The length of the silence of an old
// entry made no difference. The new code must forget the RIGHT entry. A
// neighbour that we have proved can hear us must rank above a node that we saw
// once in the path of another node.

// Fill the table with relay-only sightings at tier 0. Each one has a different
// 2-byte hash.
static void fillWithRelayOnlyPeers(MeshObserver& o, int n) {
  for (int i = 0; i < n; i++) {
    // Two hops. The first hop is the peer. The second hop holds the peer away
    // from the final position, so it never gets a direct sighting.
    std::vector<uint8_t> f = floodWithPath(
        {{(uint8_t)(0x40 + i / 256), (uint8_t)(i % 256)}, {0xFE, 0xFE}}, 2);
    o.observeRx(f.data(), (int)f.size(), 20);
  }
}

TEST(ObserverEviction, AFullTableNoLongerFreezesOutNewNodes) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  g_fake_millis = 1000;

  fillWithRelayOnlyPeers(o, MeshObserver::MAX_PEERS + 4);
  EXPECT_EQ((int)MeshObserver::MAX_PEERS, o.numPeers()) << "table stays at its cap";
  EXPECT_GT(o.evictions(), 0u) << "late arrivals must displace someone, not be dropped";
  EXPECT_EQ(0u, o.refusedInserts()) << "tier-0 entries are always evictable";
}

TEST(ObserverEviction, APeerThatHasRelayedUsOutranksOneMerelySeen) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  g_fake_millis = 1000;

  // 0xCC comes after our hash, so it received one of our transmissions
  std::vector<uint8_t> proven = floodWithPath({{0x30, 0x70}, {0xCC, 0x01}}, 2);
  o.observeRx(proven.data(), (int)proven.size(), 20);
  ASSERT_EQ(1, o.numPeers());
  ASSERT_EQ(1u, o.peer(0)->heard_us);

  // Now fill the table with sightings that have no proof. All of them are NEWER
  // than the proven one.
  g_fake_millis += 60000;
  fillWithRelayOnlyPeers(o, MeshObserver::MAX_PEERS * 2);

  bool still_there = false;
  for (int i = 0; i < o.numPeers(); i++) {
    if (o.peer(i)->hash[0] == 0xCC && o.peer(i)->heard_us > 0) still_there = true;
  }
  EXPECT_TRUE(still_there) << "a proven two-way neighbour must not be evicted for a relay sighting";
}

// An old entry becomes AVAILABLE for eviction, but it is not the first choice.
// The tier still decides. Thus an old proven neighbour lives longer than new
// relay-only sightings. The core gives it up only when the table holds nothing
// of less value.
TEST(ObserverEviction, AStaleProvenPeerYieldsOnlyWhenEverySlotIsLiveAndDirect) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  g_fake_millis = 1000;

  std::vector<uint8_t> proven = floodWithPath({{0x30, 0x70}, {0xCC, 0x01}}, 2);
  o.observeRx(proven.data(), (int)proven.size(), 20);
  ASSERT_EQ(3, o.peerTier(*o.peer(0))) << "heard_us puts it in the top tier";

  // One hour later, fill every free slot with peers that we heard directly and
  // very recently. The proven entry is then the only old entry in the table.
  g_fake_millis += MeshObserver::STALE_MS + 1000;
  for (int i = 0; i + 1 < (int)MeshObserver::MAX_PEERS; i++) {
    std::vector<uint8_t> f = floodWithPath({{0x40, (uint8_t)i}}, 2);
    o.observeRx(f.data(), (int)f.size(), 20);
  }
  ASSERT_EQ((int)MeshObserver::MAX_PEERS, o.numPeers());

  std::vector<uint8_t> newcomer = floodWithPath({{0x99, 0x99}, {0xFE, 0xFE}}, 2);
  o.observeRx(newcomer.data(), (int)newcomer.size(), 20);

  bool still_there = false;
  for (int i = 0; i < o.numPeers(); i++) {
    if (o.peer(i)->hash[0] == 0xCC) still_there = true;
  }
  EXPECT_FALSE(still_there)
      << "protection is about being CURRENT; an hour of silence must not hold a slot forever";
}

TEST(ObserverEviction, AWedgedTableRefusesAndSaysSoRatherThanLookingQuiet) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  g_fake_millis = 1000;

  // Fill every slot with peers that we heard DIRECTLY and recently, at tier 1 or
  // above. Then the core can evict nothing.
  for (int i = 0; i < MeshObserver::MAX_PEERS; i++) {
    std::vector<uint8_t> f = floodWithPath({{0x40, (uint8_t)i}}, 2);
    o.observeRx(f.data(), (int)f.size(), 20);
  }
  ASSERT_EQ((int)MeshObserver::MAX_PEERS, o.numPeers());

  uint32_t before = o.refusedInserts();
  std::vector<uint8_t> newcomer = floodWithPath({{0x99, 0x99}, {0xFE, 0xFE}}, 2);
  o.observeRx(newcomer.data(), (int)newcomer.size(), 20);
  EXPECT_GT(o.refusedInserts(), before)
      << "a wedged table must be visible as refusals, not silently lose sightings";
}

TEST(ObserverEviction, CollisionProneOneByteEntriesAreGivenUpFirst) {
  MeshObserver o;
  g_fake_millis = 1000;

  // 0x11 is a relay-only sighting. 0xFE is the final hop, so we heard it
  // directly. Both arrive as 1-byte hashes. We cannot trust that width.
  std::vector<uint8_t> narrow = floodWithPath({{0x11}, {0xFE}}, 1);
  o.observeRx(narrow.data(), (int)narrow.size(), 20);
  EXPECT_EQ(2, o.widthCount(1)) << "both path entries were only a byte wide";

  fillWithRelayOnlyPeers(o, MeshObserver::MAX_PEERS * 2);

  bool narrow_survived = false;
  for (int i = 0; i < o.numPeers(); i++) {
    if (o.peer(i)->width == 1 && o.peer(i)->hash[0] == 0x11) narrow_survived = true;
  }
  EXPECT_FALSE(narrow_survived)
      << "a 1-in-256 guess should be surrendered before a hash we can trust";
}

// -------------------------------------------------- pooled duty-cycle budget
//
// Dispatcher gives every Mesh instance its own tx_budget_ms. Behind one antenna
// that is N budgets for one transmitter. A node with three slots that believes
// it holds a 50% duty cycle transmits at 150%. Only the pool below knows what
// the NODE spent.

// In the fake radio the estimated airtime of a frame is its length in ms. Thus
// the budgets and the frame sizes use the same units everywhere.
static void sendAndRelease(FakeRadio& radio, RadioPort& p, const uint8_t* buf, int len) {
  if (!p.startSendRaw(buf, len)) return;
  radio.send_complete = true;
  p.isSendComplete();
  p.onSendFinished();
}

TEST(PooledBudget, OneBudgetForTheWholeNodeNotOnePerIdentity) {
  g_fake_millis = 0;
  Fixture f;
  f.core.setDutyCycle(9.0f, 10000);              // 10% of a 10s window = 1000 ms
  ASSERT_EQ(1000u, f.core.txBudgetMaxMs());

  uint8_t frame[200] = {0};
  for (int i = 0; i < 5; i++) sendAndRelease(f.radio, f.a, frame, 200);
  ASSERT_EQ(5u, f.radio.sent.size());
  ASSERT_EQ(0u, f.core.txBudgetMs()) << "one identity has spent the lot";

  EXPECT_FALSE(f.b.startSendRaw(frame, 200))
      << "a sibling must not arrive with a budget of its own to spend";
  EXPECT_EQ(5u, f.radio.sent.size()) << "nothing more may reach the air";
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_BUDGET));
}

TEST(PooledBudget, TheNodeNeverTransmitsMoreThanTheDutyCycleAllows) {
  g_fake_millis = 0;
  Fixture f;
  f.core.setDutyCycle(9.0f, 10000);              // 10%
  const uint32_t start_budget = f.core.txBudgetMaxMs();

  RadioPort* ports[3] = { &f.a, &f.b, &f.c };
  uint8_t frame[64] = {0};

  // Three identities transmit as much as the rules allow, for twenty simulated
  // seconds. The pool is the only limit.
  for (int step = 0; step < 2000; step++) {
    for (int i = 0; i < 3; i++) sendAndRelease(f.radio, *ports[i], frame, 64);
    g_fake_millis += 10;

    // THE RULE: the airtime is never more than the start value of one window
    // plus what the duty cycle has earned since then. A budget for each identity
    // would go past this by a factor of three in the first second.
    uint32_t allowed = start_budget + (uint32_t)(g_fake_millis / 10);
    ASSERT_LE(f.core.txChargedMs(), allowed)
        << "over-drawn at t=" << g_fake_millis << "ms";
  }
  EXPECT_GT(f.core.txChargedMs(), 0u) << "the test must actually have transmitted";
}

TEST(PooledBudget, TheBudgetRefillsAtTheDutyCycleAndIsCappedAtOneWindow) {
  g_fake_millis = 0;
  Fixture f;
  f.core.setDutyCycle(9.0f, 10000);
  uint8_t frame[200] = {0};
  for (int i = 0; i < 5; i++) sendAndRelease(f.radio, f.a, frame, 200);
  ASSERT_EQ(0u, f.core.txBudgetMs());

  g_fake_millis += 5000;
  EXPECT_EQ(500u, f.core.txBudgetMs()) << "10% of five seconds";

  g_fake_millis += 1000000;
  EXPECT_EQ(1000u, f.core.txBudgetMs()) << "a long quiet spell does not bank more than a window";
}

TEST(PooledBudget, AFrameLargerThanWhatIsLeftIsRefusedOutright) {
  g_fake_millis = 0;
  Fixture f;
  f.core.setDutyCycle(9.0f, 10000);
  uint8_t frame[200] = {0};
  for (int i = 0; i < 4; i++) sendAndRelease(f.radio, f.a, frame, 200);
  ASSERT_EQ(200u, f.core.txBudgetMs());

  // startSendRaw() is the ONE check that a dispatcher cannot pass. Its CAD-busy
  // timeout sends past isReceiving() by force after 4 s. But a refusal here
  // discards the packet. It does not let the node go past its duty cycle.
  EXPECT_FALSE(f.a.startSendRaw(frame, 201));
  EXPECT_EQ(4u, f.radio.sent.size());
  EXPECT_TRUE(f.a.startSendRaw(frame, 200)) << "exactly what is left must still fit";
}

TEST(PooledBudget, PortsAreToldToBackOffBeforeTheBudgetIsCompletelyGone) {
  g_fake_millis = 0;
  Fixture f;
  f.core.setDutyCycle(9.0f, 10000);
  uint8_t frame[190] = {0};
  for (int i = 0; i < 5; i++) sendAndRelease(f.radio, f.a, frame, 190);

  ASSERT_LT(f.core.txBudgetMs(), 100u) << "under the reserve";
  ASSERT_GT(f.core.txBudgetMs(), 0u)   << "but not empty";
  EXPECT_TRUE(f.b.isReceiving())
      << "back off politely while there is still room, rather than have a packet dropped";
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_BUDGET));
}

TEST(PooledBudget, DefaultsToTheSameFiftyPercentDispatcherAssumesWhenNobodySetsIt) {
  Fixture f;
  EXPECT_EQ(1800000u, f.core.txBudgetMaxMs())
      << "an uncapped node is the failure mode; a composition that forgets must still be capped";
}

// ------------------------------------------------------- why we could not TX

static int g_probe_reason = TXWAIT_RX_PACKET;
static int fakeBusyProbe() { return g_probe_reason; }

TEST(TxWaitReasons, AQuietBandLeavesEveryCounterAtZero) {
  g_fake_millis = 0;
  Fixture f;
  uint8_t msg[] = {1, 2, 3};
  for (int i = 0; i < 5; i++) {
    ASSERT_FALSE(f.a.isReceiving());
    sendAndRelease(f.radio, f.a, msg, 3);
  }
  // This is the point of the test. A node with no obstacle must not look like a
  // node that can never transmit.
  for (int r = 0; r < TXWAIT_NUM; r++) EXPECT_EQ(0u, f.core.txWaits(r)) << "reason " << r;
}

TEST(TxWaitReasons, ASiblingHoldingTheTransmitterIsNotConfusedWithABusyChannel) {
  g_fake_millis = 0;
  Fixture f;
  uint8_t msg[] = {1, 2, 3};
  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 3));

  EXPECT_TRUE(f.b.isReceiving());
  EXPECT_FALSE(f.b.startSendRaw(msg, 3));
  EXPECT_EQ(2u, f.core.txWaits(TXWAIT_SIBLING)) << "the deferral and the refusal are both ours";
  EXPECT_EQ(0u, f.core.txWaits(TXWAIT_RX_PACKET)) << "the band was not busy at all";
}

TEST(TxWaitReasons, ChannelBusyIsSplitIntoItsThreeUnderlyingConditions) {
  g_fake_millis = 0;
  Fixture f;
  f.radio.receiving = true;
  f.core.setChannelBusyProbe(fakeBusyProbe);

  g_probe_reason = TXWAIT_RX_PACKET; EXPECT_TRUE(f.a.isReceiving());
  g_probe_reason = TXWAIT_RSSI;      EXPECT_TRUE(f.a.isReceiving());
  g_probe_reason = TXWAIT_RSSI;      EXPECT_TRUE(f.a.isReceiving());
  g_probe_reason = TXWAIT_CAD;       EXPECT_TRUE(f.a.isReceiving());

  // "channel busy" covers three conditions: a neighbour in the middle of a
  // packet, a threshold that is too strict, and hardware CAD. Each condition
  // needs a different answer.
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_RX_PACKET));
  EXPECT_EQ(2u, f.core.txWaits(TXWAIT_RSSI));
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_CAD));
}

TEST(TxWaitReasons, WithNoProbeABusyChannelIsAttributedToPacketDetectionNotGuessed) {
  g_fake_millis = 0;
  Fixture f;
  f.radio.receiving = true;
  EXPECT_TRUE(f.a.isReceiving());
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_RX_PACKET));
}

TEST(TxWaitReasons, ASendTheDriverItselfRefusesIsCountedAsARadioFailure) {
  g_fake_millis = 0;
  Fixture f;
  uint8_t msg[] = {1, 2, 3};
  f.radio.send_ok = false;
  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_RADIO));
  EXPECT_EQ(1u, f.core.txRefused());
}

TEST(TxWaitReasons, AnExhaustedBudgetIsDistinguishableFromABusyChannel) {
  g_fake_millis = 0;
  Fixture f;
  f.core.setDutyCycle(9.0f, 10000);
  uint8_t frame[200] = {0};
  for (int i = 0; i < 5; i++) sendAndRelease(f.radio, f.a, frame, 200);

  f.radio.receiving = true;                  // the band is busy AS WELL
  EXPECT_TRUE(f.b.isReceiving());
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_BUDGET))
      << "our own duty cycle is the obstacle, and blaming the band would hide that";
  EXPECT_EQ(0u, f.core.txWaits(TXWAIT_RX_PACKET));
}

// ------------------------------------------------------ cross-slot priority
//
// The upstream code ranks packets INSIDE one Dispatcher. Routed traffic queues
// at ACTION_RETRANSMIT_DELAYED(0, d). Between identities there was no rank. The
// transmitter served the first request. Thus a busy chat slot could stay in
// front of the routed traffic of the repeater for as long as it had data.

TEST(CrossSlotPriority, AHigherRankedPortGetsTheChannelAheadOfALowerOne) {
  g_fake_millis = 1000;
  Fixture f;
  f.core.setPortPriority(0, 0);                  // port a is the repeater
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.c.startSendRaw(msg, 3));
  EXPECT_FALSE(f.a.startSendRaw(msg, 3)) << "the repeater wanted the air and did not get it";
  f.radio.send_complete = true;
  f.c.isSendComplete();
  f.c.onSendFinished();                          // the transmitter is free again

  EXPECT_TRUE(f.b.isReceiving())
      << "a chat identity must not step in front of routed traffic that is already waiting";
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_PRIORITY));
  EXPECT_FALSE(f.a.isReceiving()) << "the claimant itself is never held back";
  EXPECT_TRUE(f.a.startSendRaw(msg, 3));
}

TEST(CrossSlotPriority, PriorityIsPerPortNotHardCodedToSlotZero) {
  g_fake_millis = 1000;
  Fixture f;
  f.core.setPortPriority(2, 0);                  // give the LAST port the highest rank
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 3));
  EXPECT_FALSE(f.c.startSendRaw(msg, 3));        // c makes a claim
  f.radio.send_complete = true;
  f.a.isSendComplete();
  f.a.onSendFinished();

  EXPECT_TRUE(f.a.isReceiving()) << "port 0 yields when it is not the ranked one";
  EXPECT_TRUE(f.b.isReceiving());
  EXPECT_EQ(0u, f.core.portPriority(2));
}

TEST(CrossSlotPriority, EqualRanksDoNotYieldToEachOther) {
  g_fake_millis = 1000;
  Fixture f;                                     // every port keeps the default rank
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.c.startSendRaw(msg, 3));
  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  f.radio.send_complete = true;
  f.c.isSendComplete();
  f.c.onSendFinished();

  EXPECT_FALSE(f.b.isReceiving()) << "without a rank there is nothing to yield to";
  EXPECT_EQ(0u, f.core.txWaits(TXWAIT_PRIORITY));
}

TEST(CrossSlotPriority, TransmittingClearsTheClaimAndReleasesTheOthersAtOnce) {
  g_fake_millis = 1000;
  Fixture f;
  f.core.setPortPriority(0, 0);
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.c.startSendRaw(msg, 3));
  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  f.radio.send_complete = true;
  f.c.isSendComplete();
  f.c.onSendFinished();

  ASSERT_TRUE(f.b.isReceiving());
  sendAndRelease(f.radio, f.a, msg, 3);          // the port with the claim gets its turn
  EXPECT_FALSE(f.b.isReceiving()) << "a satisfied claim must not linger";
}

TEST(CrossSlotPriority, AClaimExpiresSoAQuietHighRankedPortCannotSilenceTheRest) {
  g_fake_millis = 1000;
  Fixture f;
  f.core.setPortPriority(0, 0);
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.c.startSendRaw(msg, 3));
  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  f.radio.send_complete = true;
  f.c.isSendComplete();
  f.c.onSendFinished();
  ASSERT_TRUE(f.b.isReceiving());

  g_fake_millis += 1500;                         // past CLAIM_TTL_MS
  EXPECT_FALSE(f.b.isReceiving())
      << "a repeater that went quiet must not hold the board off indefinitely";
}

TEST(CrossSlotPriority, ASilencedHighRankedPortHoldsNobodyBack) {
  g_fake_millis = 1000;
  Fixture f;
  f.core.setPortPriority(0, 0);
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.c.startSendRaw(msg, 3));
  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  f.radio.send_complete = true;
  f.c.isSendComplete();
  f.c.onSendFinished();
  ASSERT_TRUE(f.b.isReceiving());

  f.core.setPortActive(f.ia, false);
  EXPECT_FALSE(f.b.isReceiving()) << "a disabled identity has no say in anything";
}

TEST(CrossSlotPriority, PriorityDefersButNeverRefusesSoAForcedSendStillGetsThrough) {
  g_fake_millis = 1000;
  Fixture f;
  f.core.setPortPriority(0, 0);
  uint8_t msg[] = {1, 2, 3};

  f.radio.send_complete = false;
  ASSERT_TRUE(f.c.startSendRaw(msg, 3));
  EXPECT_FALSE(f.a.startSendRaw(msg, 3));
  f.radio.send_complete = true;
  f.c.isSendComplete();
  f.c.onSendFinished();
  ASSERT_TRUE(f.b.isReceiving());

  // The dispatcher of b has used its own 4 s CAD-busy timeout and now sends by
  // force. A refusal here discards the packet. If the core lets the send
  // through, the timeout that already exists upstream limits the starvation.
  EXPECT_TRUE(f.b.startSendRaw(msg, 3));
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_FORCED));
}

// ------------------------------------------------------------- clock sampler
// The observer turns adverts into the readings that mesh::clockEstimate works
// on, and it records the newest timestamp that this node has put on the air.
// That mark is the replay floor: see MeshObserver::sentHighWater and
// mesh::clockDecide.
namespace {
class TestClock : public mesh::RTCClock {
  uint32_t _t;
public:
  explicit TestClock(uint32_t t) : _t(t) {}
  uint32_t getCurrentTime() override { return _t; }
  void setCurrentTime(uint32_t t) override { _t = t; }
};

const uint32_t NOW_S = 2000000000UL;   // any moment above mesh::CLOCK_SET_EPOCH

// An advert that carries a timestamp, which the builder above leaves at zero.
std::vector<uint8_t> stampedAdvert(std::vector<uint8_t> pub, uint32_t ts,
                                   std::vector<std::vector<uint8_t>> hops = {}) {
  std::vector<uint8_t> f = advert(pub, "N", 0, 0, hops, 2);
  // [hdr][path_len][path][pub 32][ts 4] -- the timestamp follows the key
  size_t o = 2 + hops.size() * 2 + 32;
  for (int i = 0; i < 4; i++) f[o + i] = (uint8_t)((ts >> (8 * i)) & 0xFF);
  return f;
}
}

TEST(ClockSampler, EachPeerContributesOneReadingOfItsOwnClock) {
  Fixture f;
  TestClock clk(NOW_S);
  f.core.observer().setClock(&clk);

  f.deliver(stampedAdvert({0x11, 0x22, 0x33}, NOW_S + 40));
  f.deliver(stampedAdvert({0x44, 0x55, 0x66}, NOW_S + 41));

  mesh::ClockSample s[mesh::CLOCK_POLICY_MAX_SAMPLES];
  int n = f.core.observer().clockSamples(s, mesh::CLOCK_POLICY_MAX_SAMPLES);
  ASSERT_EQ(2, n);
  EXPECT_EQ(0, s[0].hops) << "an empty path means we heard its own transmission";
  EXPECT_EQ(40, s[0].offset_s);
  EXPECT_EQ(41, s[1].offset_s);
}

TEST(ClockSampler, TheReadingIsRecomputedAgainstWhateverOurClockReadsNow) {
  /* The ring holds what the peer SAID, never a difference from our clock. A
     step of ours must therefore change the answer, because the old difference
     would otherwise become a lie the moment convergence moved us. */
  Fixture f;
  TestClock clk(NOW_S);
  f.core.observer().setClock(&clk);
  f.deliver(stampedAdvert({0x11, 0x22, 0x33}, NOW_S + 40));

  mesh::ClockSample s[4];
  ASSERT_EQ(1, f.core.observer().clockSamples(s, 4));
  EXPECT_EQ(40, s[0].offset_s);

  clk.setCurrentTime(NOW_S + 30);            // convergence moved us 30s forward
  ASSERT_EQ(1, f.core.observer().clockSamples(s, 4));
  EXPECT_EQ(10, s[0].offset_s) << "the same reading, against the new clock";
}

TEST(ClockSampler, APeerWithNoClockOfItsOwnDoesNotVote) {
  Fixture f;
  TestClock clk(NOW_S);
  f.core.observer().setClock(&clk);
  // 15 May 2024: the VolatileRTCClock default, which is below CLOCK_SET_EPOCH.
  f.deliver(stampedAdvert({0x11, 0x22, 0x33}, 1715770351UL));

  mesh::ClockSample s[4];
  EXPECT_EQ(0, f.core.observer().clockSamples(s, 4));
}

TEST(ClockSampler, PeersThatShareOneErrorCollapseOntoOneWeightedReading) {
  /* In the 407-node survey a single group of 41 nodes held one offset. Counted
     one by one, that group would have voted 41 times. */
  Fixture f;
  TestClock clk(NOW_S);
  f.core.observer().setClock(&clk);
  f.deliver(stampedAdvert({0x11, 0x22, 0x33}, NOW_S + 200));
  f.deliver(stampedAdvert({0x44, 0x55, 0x66}, NOW_S + 200));
  f.deliver(stampedAdvert({0x77, 0x88, 0x99}, NOW_S + 200));

  mesh::ClockSample s[8];
  ASSERT_EQ(1, f.core.observer().clockSamples(s, 8));
  EXPECT_EQ(200, s[0].offset_s);
  EXPECT_EQ(3, s[0].weight) << "one reading, carrying the size of the group";
}

TEST(ClockSampler, ARelayedAdvertIsSampledWithItsHopCount) {
  Fixture f;
  TestClock clk(NOW_S);
  f.core.observer().setClock(&clk);
  f.deliver(stampedAdvert({0x11, 0x22, 0x33}, NOW_S, {{0xAA, 0x01}, {0xBB, 0x02}}));

  mesh::ClockSample s[4];
  ASSERT_EQ(1, f.core.observer().clockSamples(s, 4));
  EXPECT_EQ(2, s[0].hops) << "so the estimator can undo the journey and down-weight it";
}

TEST(ReplayFloor, TheObserverRecordsTheNewestTimestampWeHavePutOnTheAir) {
  Fixture f;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  EXPECT_EQ(0u, f.core.observer().sentHighWater()) << "nothing sent yet";

  auto mine = stampedAdvert({0x30, 0x70, 0x30, 0x70}, NOW_S);
  f.core.observer().observeTx(mine.data(), (int)mine.size(), 0);
  EXPECT_EQ(NOW_S, f.core.observer().sentHighWater());

  // A later advert raises the mark.
  auto later = stampedAdvert({0x30, 0x70, 0x30, 0x70}, NOW_S + 600);
  f.core.observer().observeTx(later.data(), (int)later.size(), 0);
  EXPECT_EQ(NOW_S + 600, f.core.observer().sentHighWater());

  /* An earlier one does not lower it. The mark is what peers hold for us, and a
     peer never forgets the highest timestamp it has seen from this node. */
  auto earlier = stampedAdvert({0x30, 0x70, 0x30, 0x70}, NOW_S + 60);
  f.core.observer().observeTx(earlier.data(), (int)earlier.size(), 0);
  EXPECT_EQ(NOW_S + 600, f.core.observer().sentHighWater());
}

TEST(ReplayFloor, AnAdvertFromSomebodyElseIsNotOurMark) {
  Fixture f;
  f.core.setPortIdentity(f.ia, SELF_KEY);
  auto theirs = stampedAdvert({0x11, 0x22, 0x33}, NOW_S);
  f.core.observer().observeTx(theirs.data(), (int)theirs.size(), 0);
  EXPECT_EQ(0u, f.core.observer().sentHighWater())
      << "only what WE originated is a mark against us";
}

// ------------------------------------------- loopback frames are not relayed
//
// The loopback must keep DELIVERING, because that is what lets a chat identity
// address the repeater and the room on the same board. It must stop the
// FORWARD, because a relay of a sibling adds no coverage: one antenna, one
// radio horizon. See LoopbackForwardGuard in helpers/SharedRadio.h.

namespace {
// A stand-in packet address. The guard takes a packet pointer as an identity
// token and never reads through it, so a distinct address is a distinct packet.
const mesh::Packet* fakePkt(int n) {
  return reinterpret_cast<const mesh::Packet*>((uintptr_t)0x1000 + n * 0x40);
}
}

TEST(SharedRadioLoopback, ADeliveredSiblingFrameIsMarkedAsLoopback) {
  Fixture f(/*loopback=*/true);
  uint8_t msg[] = {0x42, 0x43};
  ASSERT_TRUE(f.b.startSendRaw(msg, 2));
  f.b.onSendFinished();
  f.core.pump();

  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(2, take(f.a, buf)) << "delivery must survive: a chat slot reaches slot 0 this way";
  EXPECT_EQ(0x42, buf[0]);
  EXPECT_TRUE(f.a.lastRxWasLoopback());
  ASSERT_EQ(2, take(f.c, buf));
  EXPECT_TRUE(f.c.lastRxWasLoopback());
}

TEST(SharedRadioLoopback, AnOverTheAirFrameIsNotMarked) {
  Fixture f(true);
  f.deliver({0x11, 0x22});
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(2, take(f.a, buf));
  EXPECT_FALSE(f.a.lastRxWasLoopback());
}

// The mark belongs to the frame and not to the port. A port that takes a
// sibling frame and then an air frame must report the air frame correctly.
TEST(SharedRadioLoopback, TheMarkFollowsEachFrameThroughTheQueue) {
  Fixture f(true);
  uint8_t msg[] = {0xB0};
  ASSERT_TRUE(f.b.startSendRaw(msg, 1));
  f.b.onSendFinished();
  f.radio.pending_rx = {0xA1};
  f.core.pump();               // the sibling frame is queued first, then the air frame

  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(0xB0, buf[0]);
  EXPECT_TRUE(f.a.lastRxWasLoopback());
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(0xA1, buf[0]);
  EXPECT_FALSE(f.a.lastRxWasLoopback()) << "an air frame after a sibling frame is still air";
}

TEST(LoopbackForwardGuard, BlocksTheForwardOfAFrameThatCameFromASibling) {
  LoopbackForwardGuard g;
  g.onRx(fakePkt(1), true);

  g.beginProcess(fakePkt(1));
  EXPECT_TRUE(g.blocksForward());
  g.endProcess();
  EXPECT_FALSE(g.blocksForward()) << "the block must not leak into the next packet";
}

TEST(LoopbackForwardGuard, AnAirFrameIsStillForwarded) {
  LoopbackForwardGuard g;
  g.onRx(fakePkt(1), false);
  g.beginProcess(fakePkt(1));
  EXPECT_FALSE(g.blocksForward());
  g.endProcess();
}

// A flood packet can wait in the delayed inbound queue of the Dispatcher while
// other packets arrive and are decided. The mark must still be there.
TEST(LoopbackForwardGuard, AMarkSurvivesOtherTrafficArrivingAndBeingDecided) {
  LoopbackForwardGuard g;
  g.onRx(fakePkt(1), true);       // a sibling flood, now waiting on its rx delay
  g.onRx(fakePkt(2), false);      // air traffic arrives and is decided meanwhile
  g.beginProcess(fakePkt(2));
  EXPECT_FALSE(g.blocksForward());
  g.endProcess();

  g.beginProcess(fakePkt(1));
  EXPECT_TRUE(g.blocksForward());
  g.endProcess();
}

// The pool hands the same address out again. Every received packet passes
// onRx() before anything asks about it, so the new packet rewrites the entry.
TEST(LoopbackForwardGuard, ARecycledPacketAddressDoesNotInheritAMark) {
  LoopbackForwardGuard g;
  g.onRx(fakePkt(1), true);
  g.onRx(fakePkt(1), false);      // the pool gave this address to an air frame
  g.beginProcess(fakePkt(1));
  EXPECT_FALSE(g.blocksForward());
  EXPECT_EQ(0, g.numMarks());
}

TEST(LoopbackForwardGuard, ADecisionReleasesTheMark) {
  LoopbackForwardGuard g;
  g.onRx(fakePkt(1), true);
  g.beginProcess(fakePkt(1));
  g.endProcess();
  EXPECT_EQ(0, g.numMarks()) << "a mark that is never released would fill the table";

  g.beginProcess(fakePkt(1));
  EXPECT_FALSE(g.blocksForward());
}

TEST(LoopbackForwardGuard, MarkingTheSamePacketTwiceCostsOneSlot) {
  LoopbackForwardGuard g;
  g.onRx(fakePkt(1), true);
  g.onRx(fakePkt(1), true);
  EXPECT_EQ(1, g.numMarks());
}

TEST(LoopbackForwardGuard, OverflowIsCountedAndNotSilent) {
  LoopbackForwardGuard g;
  const int n = LoopbackForwardGuard::MARK_SLOTS;
  for (int i = 0; i < n + 2; i++) g.onRx(fakePkt(i), true);
  EXPECT_EQ(n, g.numMarks());
  EXPECT_EQ(2u, g.overflows()) << "a sibling frame that we may have relayed must leave evidence";

  // The oldest marks are the ones that gave way. The newest are still held.
  g.beginProcess(fakePkt(0));
  EXPECT_FALSE(g.blocksForward());
  g.endProcess();
  g.beginProcess(fakePkt(n + 1));
  EXPECT_TRUE(g.blocksForward());
}

// The whole path in one test: a chat slot transmits, the repeater slot on the
// same board receives it complete, and the repeater refuses to relay it. The
// same repeater still relays the same bytes when they come off the air.
TEST(SharedRadioLoopback, AChatSlotReachesTheRepeaterButTheRepeaterDoesNotRelayIt) {
  Fixture f(/*loopback=*/true);
  LoopbackForwardGuard repeater_guard;
  const mesh::Packet* parsed = fakePkt(7);   // what the Dispatcher of slot 0 allocates

  uint8_t chat_frame[] = {0x05, 0x00, 0x99};
  ASSERT_TRUE(f.b.startSendRaw(chat_frame, 3));
  f.b.onSendFinished();
  f.core.pump();

  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(3, take(f.a, buf)) << "the repeater must still SEE what its sibling said";
  EXPECT_EQ(0, memcmp(buf, chat_frame, 3));
  repeater_guard.onRx(parsed, f.a.lastRxWasLoopback());

  repeater_guard.beginProcess(parsed);
  EXPECT_TRUE(repeater_guard.blocksForward()) << "no relay: one antenna, one radio horizon";
  repeater_guard.endProcess();

  f.deliver({0x05, 0x00, 0x99});
  ASSERT_EQ(3, take(f.a, buf));
  repeater_guard.onRx(parsed, f.a.lastRxWasLoopback());
  repeater_guard.beginProcess(parsed);
  EXPECT_FALSE(repeater_guard.blocksForward());
}
