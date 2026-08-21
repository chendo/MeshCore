// LoraWatchdog: the node-scoped staged escalation that tells a dead radio apart
// from a quiet band. Every stage is driven here by moving a fake clock and a
// fake airtime counter, because on hardware the only way to observe it is to
// wait fifteen minutes and then have the board reboot.

#include <gtest/gtest.h>
#include <limits.h>
#include "helpers/LoraWatchdog.h"
#include "helpers/SharedRadio.h"

unsigned long g_fake_millis = 0;
FakeSerial Serial;

namespace {

const uint32_t IDLE_MS = 900000;   // the RAK3401 setting: 15 minutes

// Records what the watchdog did to the world, in order.
struct Node {
  uint32_t airtime = 1000;   // never zero: a fresh counter is not "no radio"
  int probes = 0, reinits = 0, reboots = 0;
  bool reinit_restores_airtime = true;

  static uint32_t air(void* c) { return ((Node*)c)->airtime; }
  static void probe(void* c) { ((Node*)c)->probes++; }
  static void reinit(void* c) {
    Node* n = (Node*)c;
    n->reinits++;
    if (!n->reinit_restores_airtime) return;
    n->airtime += 50;   // a radio that came back: the probe reaches the air
  }
  static void reboot(void* c) { ((Node*)c)->reboots++; }
};

struct Fixture {
  Node node;
  LoraWatchdog wd;
  Fixture(uint32_t idle_ms = IDLE_MS, unsigned long start_ms = 10000) {
    g_fake_millis = start_ms;
    wd.begin(idle_ms, &node, Node::air, Node::probe, Node::reinit, Node::reboot);
    wd.loop();   // first observation seeds the activity stamp
  }
  // One check window: exactly what the watchdog paces itself to.
  void step() { g_fake_millis += LoraWatchdog::CHECK_EVERY_MS; wd.loop(); }
  // Advance the clock and give the watchdog every check it is due.
  void advance(unsigned long ms) {
    unsigned long target = g_fake_millis + ms;
    while ((long)(g_fake_millis - target) < 0) {
      unsigned long step = LoraWatchdog::CHECK_EVERY_MS;
      if ((unsigned long)(target - g_fake_millis) < step) step = target - g_fake_millis;
      g_fake_millis += step;
      wd.loop();
    }
  }
};

// ------------------------------------------------------------------ arming

TEST(LoraWatchdog, AZeroIntervalLeavesItDisarmed) {
  Fixture f(0);
  f.advance(IDLE_MS * 4);
  EXPECT_FALSE(f.wd.isArmed());
  EXPECT_EQ(0, f.node.probes);
  EXPECT_EQ(0, f.node.reboots);
}

TEST(LoraWatchdog, WithNoAirtimeSourceItDoesNothing) {
  Node n;
  LoraWatchdog wd;
  g_fake_millis = 10000;
  wd.begin(IDLE_MS, &n, nullptr, Node::probe, Node::reinit, Node::reboot);
  EXPECT_FALSE(wd.isArmed());
  for (int i = 0; i < 100; i++) { g_fake_millis += LoraWatchdog::CHECK_EVERY_MS; wd.loop(); }
  EXPECT_EQ(0, n.probes);
  EXPECT_EQ(0, n.reboots);
}

// ------------------------------------------------------------------- pacing

TEST(LoraWatchdog, ChecksAreSpacedNotRunEveryLoop) {
  Fixture f;
  // A million calls inside one check window must not advance anything: an
  // unpaced retry loop is the failure mode this pacing exists for.
  for (int i = 0; i < 1000; i++) f.wd.loop();
  g_fake_millis += LoraWatchdog::CHECK_EVERY_MS - 1;
  for (int i = 0; i < 1000; i++) f.wd.loop();
  EXPECT_EQ(0, f.node.probes);

  f.advance(IDLE_MS);
  EXPECT_EQ(1, f.node.probes) << "one fault, one advert";
}

// ------------------------------------------------------ nothing wrong at all

TEST(LoraWatchdog, TrafficKeepsItSilentIndefinitely) {
  Fixture f;
  for (int i = 0; i < 200; i++) {   // ~100 minutes of a working radio
    f.node.airtime += 7;
    f.advance(LoraWatchdog::CHECK_EVERY_MS);
  }
  EXPECT_EQ(0, f.node.probes);
  EXPECT_EQ(0, f.node.reinits);
  EXPECT_EQ(0, f.node.reboots);
  EXPECT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());
}

TEST(LoraWatchdog, SilenceShorterThanTheIntervalIsNotAFault) {
  Fixture f;
  f.advance(IDLE_MS - LoraWatchdog::CHECK_EVERY_MS * 2);
  EXPECT_EQ(0, f.node.probes);
  EXPECT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());
}

// ------------------------------------------------------- staged escalation

TEST(LoraWatchdog, SilenceProbesTheRadioBeforeAccusingIt) {
  Fixture f;
  f.advance(IDLE_MS);
  EXPECT_EQ(1, f.node.probes);
  EXPECT_EQ(0, f.node.reinits) << "a probe must be given its grace period first";
  EXPECT_EQ(LoraWatchdog::WD_TESTING, f.wd.state());
}

TEST(LoraWatchdog, AProbeThatReachesTheAirEndsTheEscalation) {
  Fixture f;
  f.advance(IDLE_MS);
  ASSERT_EQ(1, f.node.probes);

  f.node.airtime += 60;                       // the advert went out
  f.step();
  EXPECT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());
  EXPECT_EQ(0, f.node.reinits);
  EXPECT_EQ(0, f.node.reboots);
}

TEST(LoraWatchdog, AProbeWithNoAirtimeReinitialisesTheRadio) {
  Fixture f;
  f.node.reinit_restores_airtime = false;
  f.advance(IDLE_MS);
  f.step();

  EXPECT_EQ(1, f.node.reinits);
  EXPECT_EQ(2, f.node.probes) << "the reinit is re-tested, with its own advert";
  EXPECT_EQ(0, f.node.reboots);
  EXPECT_EQ(LoraWatchdog::WD_REINITED, f.wd.state());
  EXPECT_EQ(1u, f.wd.reinits());
}

TEST(LoraWatchdog, AReinitThatWorksAbortsBeforeTheReboot) {
  Fixture f;                       // reinit_restores_airtime defaults true
  f.advance(IDLE_MS);
  f.step();
  ASSERT_EQ(1, f.node.reinits);

  f.step();
  EXPECT_EQ(0, f.node.reboots) << "airtime moved: the radio is alive";
  EXPECT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());
}

TEST(LoraWatchdog, StillSilentAfterAReinitRebootsExactlyOnce) {
  Fixture f;
  f.node.reinit_restores_airtime = false;
  f.advance(IDLE_MS);   // -> probe
  f.step();          // -> reinit + probe
  ASSERT_EQ(0, f.node.reboots);

  f.step();          // -> reboot
  EXPECT_EQ(1, f.node.reboots);
  EXPECT_EQ(2, f.node.probes) << "N adverts for one fault is the bug being avoided";
  EXPECT_EQ(1, f.node.reinits);
}

TEST(LoraWatchdog, TheRebootIsReachedOnlyThroughEveryStage) {
  // Order matters: reboot is the last resort, and a node that reboots without
  // first having tried a reinit has thrown away its only cheap recovery.
  Fixture f;
  f.node.reinit_restores_airtime = false;
  int probes_at_reinit = -1;
  f.advance(IDLE_MS);
  probes_at_reinit = f.node.probes;
  f.step();
  f.step();
  EXPECT_EQ(1, probes_at_reinit);
  EXPECT_EQ(1, f.node.reinits);
  EXPECT_EQ(1, f.node.reboots);
}

// ------------------------------------------------- aborting at every stage

TEST(LoraWatchdog, AirtimeMovingDuringTheProbeGraceAbortsTheEscalation) {
  Fixture f;
  f.advance(IDLE_MS);
  ASSERT_EQ(LoraWatchdog::WD_TESTING, f.wd.state());
  f.node.airtime += 1;                       // one byte of traffic is enough
  f.step();
  EXPECT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());
  EXPECT_EQ(0, f.node.reinits);
}

TEST(LoraWatchdog, AirtimeMovingAfterTheReinitAbortsTheReboot) {
  Fixture f;
  f.node.reinit_restores_airtime = false;
  f.advance(IDLE_MS);
  f.step();
  ASSERT_EQ(LoraWatchdog::WD_REINITED, f.wd.state());

  f.node.airtime += 1;
  f.step();
  EXPECT_EQ(0, f.node.reboots);
  EXPECT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());
}

TEST(LoraWatchdog, AfterARecoveryTheWholeEscalationIsAvailableAgain) {
  Fixture f;
  f.advance(IDLE_MS);
  f.node.airtime += 40;
  f.step();
  ASSERT_EQ(LoraWatchdog::WD_IDLE, f.wd.state());

  f.node.reinit_restores_airtime = false;
  f.advance(IDLE_MS);
  f.step();
  f.step();
  EXPECT_EQ(1, f.node.reboots);
  EXPECT_EQ(3, f.node.probes) << "one from the recovered round, two from this one";
}

// --------------------------------------------------------- millis() wrap

// Every elapsed test in the watchdog is (long)(now - then) < 0, not now < then.
// Start just below the wrap so the whole escalation straddles it: with an
// unsigned comparison the idle test would be satisfied instantly on the far
// side and the node would reboot itself for no reason. (The host's unsigned
// long is 64-bit and the device's is 32; the form of the comparison is what is
// under test, not the width.)
TEST(LoraWatchdog, TheEscalationSurvivesAMillisWrap) {
  const unsigned long start = ULONG_MAX - 60000;
  Fixture f(IDLE_MS, start);
  f.node.reinit_restores_airtime = false;

  f.advance(IDLE_MS - LoraWatchdog::CHECK_EVERY_MS);
  EXPECT_LT(g_fake_millis, start) << "the clock has wrapped";
  EXPECT_EQ(0, f.node.probes) << "a wrap is not a fifteen-minute silence";

  f.step();
  EXPECT_EQ(1, f.node.probes);
  f.step();
  EXPECT_EQ(1, f.node.reinits);
  f.step();
  EXPECT_EQ(1, f.node.reboots);
}

TEST(LoraWatchdog, PacingSurvivesAMillisWrap) {
  Fixture f(IDLE_MS, ULONG_MAX - 5);
  for (int i = 0; i < 20; i++) { g_fake_millis += 1; f.wd.loop(); }
  EXPECT_GT((uint32_t)LoraWatchdog::CHECK_EVERY_MS, 20u);
  EXPECT_EQ(0, f.node.probes) << "a wrap must not look like a due check";
}

// -------------------------------------------- the shared radio as the source

// Hydra feeds the watchdog SharedRadioCore::airtimeMs() rather than one
// Dispatcher's, because a single identity behind a shared radio only ever sees
// its own share of transmit.
class StubRadio : public mesh::Radio {
public:
  std::vector<uint8_t> pending_rx;
  int recvRaw(uint8_t* b, int sz) override {
    if (pending_rx.empty()) return 0;
    int n = (int)pending_rx.size(); if (n > sz) n = sz;
    memcpy(b, pending_rx.data(), n);
    pending_rx.clear();
    return n;
  }
  bool startSendRaw(const uint8_t*, int) override { return true; }
  bool isSendComplete() override { return true; }
  void onSendFinished() override {}
  bool isReceiving() override { return false; }
  float getLastSNR() const override { return 6.0f; }
  float getLastRSSI() const override { return -90; }
  uint32_t getEstAirtimeFor(int len) override { return (uint32_t)len * 10; }
};

TEST(LoraWatchdog, TheArbitersAirtimeCountsEveryIdentitysTraffic) {
  StubRadio radio;
  SharedRadioCore core(radio);
  RadioPort a, b;
  core.addPort(a); core.addPort(b);

  EXPECT_EQ(0u, core.airtimeMs());
  radio.pending_rx = {0x11, 0x22, 0x33, 0x44};
  core.pump();
  EXPECT_EQ(40u, core.rxAirtimeMs());

  uint8_t frame[10] = {0};
  ASSERT_TRUE(a.startSendRaw(frame, 10));
  a.onSendFinished();
  ASSERT_TRUE(b.startSendRaw(frame, 5));
  b.onSendFinished();
  EXPECT_EQ(150u, core.txAirtimeMs()) << "both identities, one radio";
  EXPECT_EQ(190u, core.airtimeMs());
}

TEST(LoraWatchdog, DrivenFromTheArbiterItRebootsOnlyWhenTheRadioIsReallyDead) {
  StubRadio radio;
  SharedRadioCore core(radio);
  RadioPort a;
  core.addPort(a);

  struct Ctx { SharedRadioCore* core; int reboots = 0; } ctx{&core};
  LoraWatchdog wd;
  g_fake_millis = 10000;
  wd.begin(IDLE_MS, &ctx,
           [](void* c) { return ((Ctx*)c)->core->airtimeMs(); },
           [](void*) {},   // probe: nothing reaches the air, the radio is dead
           [](void*) {},
           [](void* c) { ((Ctx*)c)->reboots++; });

  for (int i = 0; i < 40; i++) {   // 20 minutes of a working radio
    radio.pending_rx = {0x11, 0x22};
    core.pump();
    g_fake_millis += LoraWatchdog::CHECK_EVERY_MS;
    wd.loop();
  }
  EXPECT_EQ(0, ctx.reboots);

  int checks = 0;
  for (int i = 0; i < 60 && ctx.reboots == 0; i++) {   // then nothing at all
    core.pump();
    g_fake_millis += LoraWatchdog::CHECK_EVERY_MS;
    wd.loop();
    checks++;
  }
  // board.reboot() does not return on hardware, so "once" is about how many
  // watchdogs the node has, not about latching.
  EXPECT_EQ(1, ctx.reboots);
  EXPECT_GT(checks * (int)LoraWatchdog::CHECK_EVERY_MS, (int)IDLE_MS)
      << "it waited out the full idle interval before doing anything drastic";
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
