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

TEST(RelayConfirm, AOneByteMatchConfirmsOurTransmit) {
  Fixture f;
  g_fake_millis = 1000;
  f.core.setPortIdentity(f.ia, SELF_KEY);

  std::vector<uint8_t> ours{FLOOD_HDR, 0x00, 0xAA};
  ASSERT_TRUE(f.a.startSendRaw(ours.data(), ours.size()));
  f.a.onSendFinished();
  EXPECT_EQ(1u, f.core.floodsSent(f.ia));
  EXPECT_EQ(0u, f.core.floodsConfirmed(f.ia));

  g_fake_millis += 3000;
  f.deliver(floodWithPath({{0x30}}, 1));          // relayed, carrying our hash
  EXPECT_EQ(1u, f.core.floodsConfirmed(f.ia)) << "a neighbour passed our packet on";
  EXPECT_EQ(1u, f.core.confirmsByWidth(1));
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
    f.deliver(floodWithPath({{0x30}}, 1));
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
