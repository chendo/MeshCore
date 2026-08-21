// The arbiter built the way it ships: PKT_TRACE_ENTRIES undefined, so the
// packet trace is compiled out. Everything the node actually runs on — the
// counters, the node-wide airtime the LoRa watchdog reads, the duty pool — must
// be unaffected, and everything that reads the trace must report "nothing"
// rather than stale rows or a build failure.

#include <gtest/gtest.h>
#include "helpers/SharedRadio.h"

unsigned long g_fake_millis = 0;
FakeSerial Serial;

#if PKT_TRACE_ENTRIES
#error "this suite exists to build with the trace off"
#endif

namespace {

class FakeRadio : public mesh::Radio {
public:
  std::vector<uint8_t> pending_rx;
  uint32_t recv_errors = 0;
  int recvRaw(uint8_t* bytes, int sz) override {
    if (pending_rx.empty()) return 0;
    int n = (int)pending_rx.size(); if (n > sz) n = sz;
    memcpy(bytes, pending_rx.data(), n);
    pending_rx.clear();
    return n;
  }
  bool startSendRaw(const uint8_t*, int) override { return true; }
  bool isSendComplete() override { return true; }
  void onSendFinished() override {}
  bool isReceiving() override { return false; }
  float getLastSNR() const override { return 5.5f; }
  float getLastRSSI() const override { return -88; }
  uint32_t getEstAirtimeFor(int len) override { return (uint32_t)len * 10; }
};

struct Fixture {
  FakeRadio radio;
  SharedRadioCore core{radio};
  RadioPort a, b;
  Fixture() { core.addPort(a); core.addPort(b); }
  void deliver(std::vector<uint8_t> f) { radio.pending_rx = std::move(f); core.pump(); }
};

TEST(TraceDisabled, TheRingIsGoneNotMerelyEmpty) {
  EXPECT_EQ(0, (int)SharedRadioCore::PKT_LOG_SIZE);
  EXPECT_FALSE(SharedRadioCore::pktTraceEnabled());
  // A zero-length array would still occupy its alignment and still link the
  // code that writes it. Nothing of the ring may survive: the whole arbiter has
  // to be smaller than the buffer alone would have been at the full depth.
  EXPECT_LT(sizeof(SharedRadioCore), (size_t)48 * sizeof(PktLogEntry));
}

TEST(TraceDisabled, ReadersGetNothingRatherThanGarbage) {
  Fixture f;
  f.deliver({0x11, 0x22, 0x33});
  uint8_t frame[8] = {0};
  ASSERT_TRUE(f.a.startSendRaw(frame, 8));
  f.a.onSendFinished();

  PktLogEntry entries[8];
  memset(entries, 0xAA, sizeof(entries));   // poison: a reader must not see this back
  EXPECT_EQ(0, f.core.pktLogCopy(entries, 8, 0));
  EXPECT_EQ(0u, f.core.pktLogSeq());
  EXPECT_EQ(0xAA, entries[0].raw[0]) << "the buffer was never written to";
}

TEST(TraceDisabled, ACursorFromAPreviousBuildIsHarmless) {
  // stats readers keep a seq cursor across calls; one carried over from a build
  // that had the trace must not walk a ring that no longer exists.
  Fixture f;
  f.deliver({0x11, 0x22, 0x33});
  PktLogEntry entries[4];
  EXPECT_EQ(0, f.core.pktLogCopy(entries, 4, 999999));
  EXPECT_EQ(0, f.core.pktLogCopy(entries, 0, 0));
}

TEST(TraceDisabled, ThePacketCountersStillWork) {
  Fixture f;
  f.deliver({0x11, 0x22, 0x33});
  f.deliver({0x44, 0x55});
  uint8_t frame[8] = {0};
  ASSERT_TRUE(f.a.startSendRaw(frame, 8));
  f.a.onSendFinished();

  EXPECT_EQ(2u, f.core.rxTotal());
  EXPECT_EQ(1u, f.core.txTotal());
}

TEST(TraceDisabled, ContentionAndFailureCountersStillWork) {
  Fixture f;
  uint8_t frame[8] = {0};
  ASSERT_TRUE(f.a.startSendRaw(frame, 8));      // a holds the transmitter
  EXPECT_FALSE(f.b.startSendRaw(frame, 8));
  EXPECT_EQ(1u, f.core.txContention());
  EXPECT_EQ(1u, f.core.txContentionFor(1));
  EXPECT_EQ(1u, f.core.txWaits(TXWAIT_SIBLING));
}

TEST(TraceDisabled, TheNodeWideAirtimeIsUnaffected) {
  // This is the LoRa watchdog's only input. It was computed inside the trace
  // writer, so it is exactly the thing a careless gating would have removed.
  Fixture f;
  EXPECT_EQ(0u, f.core.airtimeMs());
  f.deliver({0x11, 0x22, 0x33, 0x44});
  EXPECT_EQ(40u, f.core.rxAirtimeMs());

  uint8_t frame[10] = {0};
  ASSERT_TRUE(f.a.startSendRaw(frame, 10));
  f.a.onSendFinished();
  EXPECT_EQ(100u, f.core.txAirtimeMs());
  EXPECT_EQ(140u, f.core.airtimeMs());
}

TEST(TraceDisabled, RxErrorsAreStillNoticedEvenThoughTheyAreNotRecorded) {
  Fixture f;
  static FakeRadio* s_radio;
  s_radio = &f.radio;
  f.core.setRxErrorCounter([]() { return s_radio->recv_errors; });
  f.radio.recv_errors = 3;
  f.core.pump();
  // Nothing to assert on the trace; what matters is that pump() neither
  // crashed nor spun on the error counter it can no longer log.
  PktLogEntry e[4];
  EXPECT_EQ(0, f.core.pktLogCopy(e, 4, 0));
  f.core.pump();
  EXPECT_EQ(0, f.core.pktLogCopy(e, 4, 0));
}

TEST(TraceDisabled, TheDutyCyclePoolIsUnaffected) {
  Fixture f;
  f.core.setDutyCycle(1.0f, 3600000);          // 50%
  uint32_t before = f.core.txBudgetMs();
  uint8_t frame[20] = {0};
  ASSERT_TRUE(f.a.startSendRaw(frame, 20));
  f.a.onSendFinished();
  EXPECT_EQ(200u, f.core.txChargedMs());
  EXPECT_EQ(before - 200u, f.core.txBudgetMs());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
