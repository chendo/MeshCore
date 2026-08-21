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

TEST(SharedRadio, ASlowPortStillReceivesEveryFrameInOrder) {
  Fixture f;
  f.deliver({0xAA});
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));

  // A second frame arrives while b and c are still behind. It must be taken off
  // the radio immediately — waiting for them is how packets were lost — but the
  // laggards must still be handed the frames in arrival order.
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

// The whole point of the queue: the radio's own buffer holds ONE packet, so
// anything not read out before the next one lands is gone. Draining must never
// be gated on the identities keeping up.
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

  // what survives is the NEWEST run of frames — those are still propagating
  uint8_t buf[MAX_TRANS_UNIT];
  ASSERT_EQ(1, take(f.a, buf));
  EXPECT_EQ(over, buf[0]);
}

// Frames retire as soon as every listening port has taken them, so a steady
// stream never accumulates.
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

// ------------------------------------------------- shared collision avoidance
// Each identity's dispatcher pushes its own CAD / interference-threshold prefs
// at the radio every couple of seconds. There is only one radio, so the core
// has to reconcile them rather than let the last writer win.

TEST(SharedRadioPolicy, CADStaysOnIfAnyIdentityWantsIt) {
  Fixture f;
  f.a.setCADEnabled(true);
  EXPECT_TRUE(f.radio.cad);
  f.b.setCADEnabled(false);            // b's prefs must not disable it for a
  EXPECT_TRUE(f.radio.cad);
  f.a.setCADEnabled(false);            // now nobody wants it
  EXPECT_FALSE(f.radio.cad);
}

TEST(SharedRadioPolicy, TheMostCautiousThresholdWins) {
  Fixture f;
  f.a.triggerNoiseFloorCalibrate(12);
  EXPECT_EQ(12, f.radio.threshold);
  f.b.triggerNoiseFloorCalibrate(6);   // defers to weaker signals -> more cautious
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

  // all three dispatchers ask within the same window: one calibration, not three
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
  f.b.triggerNoiseFloorCalibrate(4);   // inside the rate limit, but it matters
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

// Coding rate is not one number: what we transmit at is ours to know, but what
// we receive at belongs to the sender, who puts it in the LoRa header. Mixing
// the two up would quietly hide the interesting case — a neighbour on a
// different preset — behind our own setting.
uint8_t g_fake_rx_cr = 0;

TEST(SharedRadioTrace, ReceivesCarryTheSendersCodingRateAndTransmitsOurs) {
  Fixture f;
  f.core.setCodingRate(5);
  f.core.setRxCodingRateFn([]() -> uint8_t { return g_fake_rx_cr; });
  g_fake_rx_cr = 8;                        // the sender is running 4/8, we are not
  f.deliver({0x10, 0x20});
  uint8_t msg[] = {0x30};
  f.a.startSendRaw(msg, 1);

  PktLogEntry entries[SharedRadioCore::PKT_LOG_SIZE];
  int n = f.core.pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, 0);
  ASSERT_EQ(2, n);
  EXPECT_EQ(8, entries[0].cr) << "a receive is labelled with the CR its header carried";
  EXPECT_EQ(5, entries[1].cr) << "a transmit is labelled with the CR we send at";
}

// Airtime is the number the channel-occupancy view is built on, so a receive
// has to be costed at the CR it was actually sent at. FakeRadio bills 1 ms per
// byte for our own settings; the CR-aware hook bills the sender's CR instead.
TEST(SharedRadioTrace, ReceivedAirtimeIsPricedAtTheSendersCodingRate) {
  Fixture f;
  f.core.setCodingRate(5);
  f.core.setRxCodingRateFn([]() -> uint8_t { return g_fake_rx_cr; });
  f.core.setRxAirtimeFn([](int len, uint8_t cr) -> uint32_t { return (uint32_t)len * cr; });
  g_fake_rx_cr = 8;
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
  f.core.setCodingRate(9);                 // not a 4/x denominator: refuse it
  EXPECT_EQ(0, f.core.codingRate());
  f.core.setCodingRate(7);
  f.core.setRxCodingRateFn([]() -> uint8_t { return g_fake_rx_cr; });
  g_fake_rx_cr = 0;                        // radio could not report one
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

// The CR lives in the LoRa header, so a CRC failure — good header, mangled
// payload — still knows it. A header failure does not: the modem is still
// holding the CR of whatever it decoded LAST, and billing this packet with it
// would invent a fact. The radio here always answers 4/8; only the error code
// decides whether that answer means anything.
TEST(SharedRadioCorrupt, ACrcFailureKeepsItsCodingRateButAHeaderFailureCannot) {
  static const uint8_t damaged[] = {0x11, 0xDE, 0xAD};
  auto trace_one = [](int16_t code) {
    Fixture f;
    g_bad_payload = damaged; g_bad_len = sizeof(damaged);
    g_err_count = 0; g_err_code = code;
    f.core.setRxCodingRateFn([]() -> uint8_t { return 8; });   // stale-but-present
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

// pump() deliberately refuses to touch the radio while a transmit is in
// flight, so a send that never completes takes the WHOLE BOARD off air — every
// identity goes deaf, not just the sender. This happened in the field: a
// repeater advert held the transmitter for 3.4 hours and nothing was received
// in that time. The arbiter must take the radio back.

TEST(SharedRadioWatchdog, ATransmitThatNeverCompletesIsForceReleased) {
  Fixture f;
  g_fake_millis = 1000;
  uint8_t msg[] = {1, 2, 3};
  f.radio.send_complete = false;                 // TX-done will never arrive
  ASSERT_TRUE(f.a.startSendRaw(msg, 3));

  // radio stays untouched for a normal in-flight transmit
  g_fake_millis += 2000;
  f.radio.pending_rx = {0xAA};
  f.core.pump();
  EXPECT_FALSE(f.radio.pending_rx.empty()) << "must not disturb a live transmit";
  EXPECT_EQ(0u, f.core.txStuck());

  // ...but not forever
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
  f.core.pump();                                  // watchdog fires

  EXPECT_TRUE(f.b.startSendRaw(msg, 1)) << "another identity can transmit again";
  f.radio.send_complete = true;
  f.b.isSendComplete();
  f.b.onSendFinished();                           // let b's send finish normally

  f.deliver({0x42});                              // and reception works
  uint8_t buf[MAX_TRANS_UNIT];
  EXPECT_EQ(1, take(f.b, buf));
}

TEST(SharedRadioWatchdog, ForcedReleaseIsLoggedWithHowLongItWasHeld) {
  Fixture f;
  g_fake_millis = 1000;
  uint8_t msg[] = {1};
  f.radio.send_complete = false;
  ASSERT_TRUE(f.a.startSendRaw(msg, 1));
  g_fake_millis += 30000;                         // held 30s
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
    g_fake_millis += 700;                         // realistic airtime
    ASSERT_TRUE(f.a.isSendComplete());
    f.a.onSendFinished();
    f.core.pump();
  }
  EXPECT_EQ(0u, f.core.txStuck()) << "the watchdog must not fire on healthy traffic";
}

// ---------------------------------------------------- radio health watchdog

// The real field failure: the transceiver wedged, every RadioLib call started
// failing, and the board sat deaf for 3.4 hours with no error recorded
// anywhere — sends were refused silently and the noise floor simply froze.
namespace { int g_reinits = 0; }

TEST(SharedRadioHealth, ARefusedSendIsCountedAndTraced) {
  Fixture f;
  f.radio.send_ok = false;                      // radio rejects everything
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

  f.deliver({0x01});                            // a packet arrives: radio is alive
  uint8_t buf[MAX_TRANS_UNIT];
  take(f.a, buf); take(f.b, buf); take(f.c, buf);

  g_fake_millis += 300000;                      // 5 min quiet — normal
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
  g_fake_millis += 1000000;                     // long boot with no traffic yet
  f.core.pump();
  EXPECT_EQ(0, g_reinits) << "cannot judge a radio that has never heard anything";
}

// ------------------------------------------------------- relay confirmation

// Proof that a neighbour actually heard us: when someone relays a flood we
// sent, they append their own hash and rebroadcast — so the packet coming back
// past us still carries OUR hash in its path.
namespace {
// header byte: route=FLOOD(1), type=TXT_MSG(2) -> (2<<2)|1
const uint8_t FLOOD_HDR = (2 << 2) | 1;
// build a flood frame whose path is the given hop hashes (hash width = sz)
std::vector<uint8_t> floodWithPath(std::vector<std::vector<uint8_t>> hops, uint8_t sz) {
  std::vector<uint8_t> f{FLOOD_HDR};
  f.push_back((uint8_t)(((sz - 1) << 6) | hops.size()));
  for (auto& h : hops) for (uint8_t i = 0; i < sz; i++) f.push_back(h[i]);
  f.push_back(0xAA);   // payload
  return f;
}
const uint8_t SELF_KEY[32] = {0x30, 0x70, 0x30, 0x70};   // our pubkey prefix
}

// A 1-byte hash collides once every 256 packets, so seeing "our" hash in a path
// at that width is not evidence anyone relayed us. It is tallied, but it must
// not confirm the transmit.
TEST(RelayConfirm, AOneByteMatchIsCountedButDoesNotConfirm) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);

  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  ASSERT_TRUE(f.a.startSendRaw(ours.data(), ours.size()));
  f.a.onSendFinished();
  EXPECT_EQ(1u, f.core.floodsSent(f.ia));

  g_fake_millis += 3000;
  f.deliver(floodWithPath({{0x30}}, 1));          // 1-byte "match" of our hash
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
  f.deliver(floodWithPath({{0x99}, {0xAB}}, 1));   // somebody else's traffic
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
    f.deliver(floodWithPath({{0x30, 0x70}}, 2));   // 2-byte: a real confirmation
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
// Every forwarder appends its hash to the END of the path, so position carries
// meaning: the last entry transmitted the frame we received, and the entry
// following one of ours received our transmission.

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

  // 2-byte path: ours, then theirs -> they forwarded our transmission
  f.deliver(floodWithPath({{0x30, 0x70}, {0xBB, 0x02}}, 2));
  const auto* p = f.core.peer(f.core.numPeers() - 1);
  EXPECT_EQ(1u, p->heard_us);
  EXPECT_EQ(0u, p->heard_us_1b);
  EXPECT_EQ(1, f.core.confirmedPeerCount());

  // same shape at 1 byte must NOT count as a confirmation
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

// The originator picks the hash width, so the same node arrives at 1 and 2
// bytes. A narrower sighting must fold into the entry it uniquely matches
// rather than creating a phantom second node.
TEST(Peers, ANarrowerSightingMergesIntoTheKnownNode) {
  Fixture f;
  f.deliver(floodWithPath({{0xAB, 0xCD}}, 2));
  ASSERT_EQ(1, f.core.numPeers());
  EXPECT_EQ(2, f.core.peer(0)->width);

  f.deliver(floodWithPath({{0xAB}}, 1));           // same node, 1-byte path
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

// If a 1-byte prefix matches two known nodes there is no way to tell which one
// sent it, so it must be attributed to neither.
TEST(Peers, AnAmbiguousPrefixIsAttributedToNobody) {
  Fixture f;
  f.deliver(floodWithPath({{0xAB, 0x11}}, 2));
  f.deliver(floodWithPath({{0xAB, 0x22}}, 2));
  ASSERT_EQ(2, f.core.numPeers());
  uint32_t before0 = f.core.peer(0)->direct_rx, before1 = f.core.peer(1)->direct_rx;

  f.deliver(floodWithPath({{0xAB}}, 1));           // matches both
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
  f.push_back(0x10 | 0x80);                                   // has latlon + name
  for (int i = 0; i < 4; i++) f.push_back((lat_e6 >> (8*i)) & 0xFF);
  for (int i = 0; i < 4; i++) f.push_back((lon_e6 >> (8*i)) & 0xFF);
  for (const char* c = name; *c; c++) f.push_back((uint8_t)*c);
  return f;
}
}

TEST(PeerIdentity, ADirectAdvertRegistersANodeThatNeverForwards) {
  Fixture f;
  // empty path => we received the originator's own transmission
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
  f.deliver(floodWithPath({{0xDE, 0xAD}}, 2));       // seen forwarding, anonymous
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
  // three forwarders => originator is 4 hops out, past the near cutoff
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
// Hop depth and payload type are the cheapest useful observability there is:
// they say whether we sit among close neighbours or on the edge of a deep mesh,
// and what kind of traffic actually passes.

TEST(Observer, HopDepthOfReceivedTrafficIsBucketed) {
  Fixture f;
  f.deliver(floodWithPath({}, 2));                       // 0 hops: straight off a radio
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

// The observer must work with no arbiter at all — that is the whole point of
// lifting it out, so a single-identity repeater can use the same code.
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

// Relay confirmation must work with no arbiter, since that is the whole reason
// it moved: a single-identity repeater needs it as much as the multi board.
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

  // a direct (non-flood) send can never be relayed, so it isn't tracked
  std::vector<uint8_t> direct{(uint8_t)((2 << 2) | 2), 0x00, 0xBB};
  o.observeTx(direct.data(), (int)direct.size());
  EXPECT_EQ(1u, o.floodsSent()) << "only floods are candidates";
}

// A returning echo is only meaningful if we transmitted recently. Our hash sits
// in the path of every packet we ever forwarded, so a wide window credits
// whichever transmit is newest rather than the one that actually came back.
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
  // still tallied as a width observation, just not credited
  EXPECT_EQ(2u, o.confirmsByWidth(2));
}

TEST(Observer, ConfirmationWindowIsTunable) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  o.setConfirmWindow(1000);                               // the tight end of 1-5s
  g_fake_millis = 10000;
  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  o.observeTx(ours.data(), (int)ours.size());

  g_fake_millis += 2500;
  std::vector<uint8_t> back = floodWithPath({{0x30, 0x70}}, 2);
  o.observeRx(back.data(), (int)back.size(), 20);
  EXPECT_EQ(0u, o.floodsConfirmed()) << "2.5s is outside a 1s window";
}

// ---- peer table eviction ---------------------------------------------------
// The table used to freeze solid once full: every slot taken meant no new node
// could ever be recorded again, however long an incumbent had been silent. What
// replaces that has to forget the RIGHT entry — a neighbour we have proven can
// hear us must outrank one glimpsed once in somebody else's path.

// Fill with relay-only sightings (tier 0), each a distinct 2-byte hash.
static void fillWithRelayOnlyPeers(MeshObserver& o, int n) {
  for (int i = 0; i < n; i++) {
    // two hops: the first is the peer, the second keeps it off the final-hop
    // position so it never earns a direct sighting
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

  // 0xCC follows our hash, so it demonstrably received one of our transmissions
  std::vector<uint8_t> proven = floodWithPath({{0x30, 0x70}, {0xCC, 0x01}}, 2);
  o.observeRx(proven.data(), (int)proven.size(), 20);
  ASSERT_EQ(1, o.numPeers());
  ASSERT_EQ(1u, o.peer(0)->heard_us);

  // now flood the table with unproven sightings, all NEWER than the proven one
  g_fake_millis += 60000;
  fillWithRelayOnlyPeers(o, MeshObserver::MAX_PEERS * 2);

  bool still_there = false;
  for (int i = 0; i < o.numPeers(); i++) {
    if (o.peer(i)->hash[0] == 0xCC && o.peer(i)->heard_us > 0) still_there = true;
  }
  EXPECT_TRUE(still_there) << "a proven two-way neighbour must not be evicted for a relay sighting";
}

// Staleness makes a good peer ELIGIBLE, it does not make it the first choice:
// tier still decides, so a stale proven neighbour outlives fresh relay-only
// sightings and is surrendered only when the table holds nothing cheaper.
TEST(ObserverEviction, AStaleProvenPeerYieldsOnlyWhenEverySlotIsLiveAndDirect) {
  MeshObserver o;
  o.addSelfKey(SELF_KEY);
  g_fake_millis = 1000;

  std::vector<uint8_t> proven = floodWithPath({{0x30, 0x70}, {0xCC, 0x01}}, 2);
  o.observeRx(proven.data(), (int)proven.size(), 20);
  ASSERT_EQ(3, o.peerTier(*o.peer(0))) << "heard_us puts it in the top tier";

  // An hour on, fill every remaining slot with peers heard directly and just
  // now, leaving the proven entry as the only stale one in the table.
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

  // fill every slot with peers heard DIRECTLY and recently (tier >= 1), so
  // nothing is eligible for eviction
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

  // 0x11 is a relay-only sighting, 0xFE is the final hop so it is heard direct;
  // both arrive as 1-byte hashes, which is the width we cannot trust
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
