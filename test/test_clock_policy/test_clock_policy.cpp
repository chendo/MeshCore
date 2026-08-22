#include <gtest/gtest.h>
#include "helpers/ClockPolicy.h"
#include <MeshCore.h>
#include <vector>

using namespace mesh;

// A node somewhere in 2033. Any value above CLOCK_SET_EPOCH does the same job.
static const uint32_t SET_CLOCK = 2000000000UL;
// The 15 May 2024 that VolatileRTCClock starts every board on.
static const uint32_t VOLATILE_DEFAULT = 1715770351UL;

static ClockSample direct(int32_t off, uint8_t weight = 1) {
  ClockSample s; s.offset_s = off; s.hops = 0; s.weight = weight; return s;
}
static ClockSample relayed(int32_t off, uint8_t hops, uint8_t weight = 1) {
  ClockSample s; s.offset_s = off; s.hops = hops; s.weight = weight; return s;
}

// A node that has been up a long time, has never been told the time by a
// person, and has a full hour of slew allowance saved.
static ClockContext restedContext(uint32_t now_s = SET_CLOCK) {
  ClockContext c;
  c.now_s = now_s;
  c.since_move_ms = CLOCK_SLEW_WINDOW_MS;
  c.admin_hold_ms = 0;
  c.sent_high_s = 0;
  return c;
}

static ClockEstimate estimate(const std::vector<ClockSample>& v,
                              uint8_t min_sources = CLOCK_MIN_SOURCES,
                              uint16_t hop_delay_ms = CLOCK_HOP_DELAY_DEFAULT_MS) {
  return clockEstimate(v.data(), (int)v.size(), min_sources, hop_delay_ms);
}

// ---- how many sources it takes ---------------------------------------------

TEST(ClockQuorum, OneSourceDoesNotMoveTheClock) {
  auto est = estimate({direct(300)});
  EXPECT_FALSE(est.valid);
  EXPECT_EQ(1, est.n_used);

  auto d = clockDecide(est, restedContext());
  EXPECT_EQ(CLOCK_HOLD, d.action);
  EXPECT_EQ(CLOCK_HOLD_NO_QUORUM, d.hold);
  EXPECT_EQ(0, d.apply_s);
}

TEST(ClockQuorum, TwoSourcesAreStillNotEnoughForASetClock) {
  auto est = estimate({direct(300), direct(301)});
  EXPECT_FALSE(est.valid);
  EXPECT_EQ(2, est.n_used);
}

TEST(ClockQuorum, NoSamplesAtAllIsAHoldAndNotACrash) {
  auto est = clockEstimate(NULL, 0);
  EXPECT_FALSE(est.valid);
  EXPECT_EQ(0, est.n_seen);
  EXPECT_EQ(CLOCK_HOLD_NO_QUORUM, clockDecide(est, restedContext()).hold);
}

// ---- agreement, and the refusal to act without it ---------------------------

TEST(ClockAgreement, ThreeSourcesThatAgreeDoMoveTheClock) {
  auto est = estimate({direct(100), direct(102), direct(101)});
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(3, est.n_used);
  EXPECT_EQ(100, est.agree_pct);

  auto d = clockDecide(est, restedContext());
  EXPECT_EQ(CLOCK_SLEW, d.action);
  EXPECT_GT(d.apply_s, 0);
}

TEST(ClockAgreement, SourcesThatDisagreeDoNotMoveTheClock) {
  // Three nodes, three beliefs, no cluster. This is the rule that stops a
  // scatter of multi-hop readings walking a known-good clock away.
  auto est = estimate({direct(-200), direct(0), direct(300)});
  EXPECT_FALSE(est.valid);
  EXPECT_EQ(1, est.n_used) << "no two of them are inside the cluster band";
  EXPECT_EQ(CLOCK_HOLD_NO_QUORUM, clockDecide(est, restedContext()).hold);
}

TEST(ClockAgreement, AnEvenSplitPicksNeitherSideAndActsOnNothing) {
  /* The failure that a median cannot see. Four nodes say one thing and four say
     another, 300s apart. A median lands between them, on a value that not one
     node holds, and reports full agreement while it does so. */
  std::vector<ClockSample> v;
  for (int i = 0; i < 4; i++) v.push_back(direct(0));
  for (int i = 0; i < 4; i++) v.push_back(direct(300));

  auto est = estimate(v);
  EXPECT_FALSE(est.valid);
  EXPECT_EQ(50, est.agree_pct);
  EXPECT_EQ(4, est.n_used) << "it chose a side, it did not average the two";
  EXPECT_EQ(0, est.offset_s);
}

TEST(ClockAgreement, AClearMajorityWinsAndTheAnswerIsAValueSomebodyHolds) {
  std::vector<ClockSample> v;
  for (int i = 0; i < 6; i++) v.push_back(direct(0));
  for (int i = 0; i < 4; i++) v.push_back(direct(300));

  auto est = estimate(v);
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(60, est.agree_pct);
  EXPECT_EQ(0, est.offset_s) << "never a midpoint between the two groups";
}

// ---- the older-end bias -----------------------------------------------------

TEST(ClockOlderEnd, ASpreadOfReadingsLandsTowardTheOlderEnd) {
  // Four readings across 12s. The median is 6. The answer must sit below it,
  // so the node lands behind the consensus and corrects forward from there.
  auto est = estimate({direct(0), direct(4), direct(8), direct(12)});
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(4, est.n_used);
  EXPECT_LT(est.offset_s, 6) << "the median would be 6";
  EXPECT_GE(est.offset_s, 0) << "and never below the oldest reading";
}

TEST(ClockOlderEnd, ReadingsThatAgreeExactlyGetNoBiasAtAll) {
  // The bias scales with the disagreement. With none, there is none.
  auto est = estimate({direct(50), direct(50), direct(50)});
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(50, est.offset_s);
  EXPECT_EQ(0, est.spread_s);
}

TEST(ClockOlderEnd, TheBiasIsBoundedByTheClusterBand) {
  /* A cluster cannot be wider than the band that selected it, so the cost of
     the bias is bounded whatever the readings look like. A flat scatter across
     80s is also the case where no belief holds a majority, so the estimate is
     correctly refused as well. */
  std::vector<ClockSample> v;
  for (int i = -40; i <= 40; i += 2) v.push_back(direct(i));
  auto est = estimate(v);
  EXPECT_LE(est.spread_s, 2 * CLOCK_CLUSTER_BAND_S);
  EXPECT_FALSE(est.valid) << "a flat scatter has no majority to act on";
  EXPECT_LT(est.agree_pct, CLOCK_MIN_AGREE_PCT);
}

TEST(ClockOlderEnd, TwoEquallyDenseClustersPickTheOlderOne) {
  std::vector<ClockSample> v;
  for (int i = 0; i < 3; i++) v.push_back(direct(-100));
  for (int i = 0; i < 3; i++) v.push_back(direct(100));
  auto est = estimate(v);
  EXPECT_EQ(-100, est.offset_s);
}

// ---- hops: corrected, and worth less ----------------------------------------

TEST(ClockHops, ARelayedReadingIsCorrectedForItsJourney) {
  // Two hops at the 1500ms default is a 3s correction. The reading arrives
  // late by the length of its journey, so the correction moves it forward.
  EXPECT_EQ(3, clockHopCorrectionS(2, CLOCK_HOP_DELAY_DEFAULT_MS));

  auto raw = estimate({direct(-13), direct(-13), direct(-13)});
  auto hopped = estimate({relayed(-13, 2), relayed(-13, 2), relayed(-13, 2)});
  ASSERT_TRUE(raw.valid);
  ASSERT_TRUE(hopped.valid);
  EXPECT_EQ(-13, raw.offset_s);
  EXPECT_EQ(-10, hopped.offset_s) << "the same reading, with its journey undone";
}

TEST(ClockHops, TheMeasuredHopDelayIsWhatGetsApplied) {
  auto est = estimate({relayed(0, 4), relayed(0, 4), relayed(0, 4)}, CLOCK_MIN_SOURCES, 800);
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(3, est.offset_s) << "(4 * 800 + 500) / 1000";
}

TEST(ClockHops, ARelayedReadingIsWorthLessThanADirectOne) {
  EXPECT_EQ(CLOCK_WEIGHT_DIRECT, clockSampleWeight(0, 1));
  for (uint8_t h = 1; h <= CLOCK_MAX_HOPS; h++) {
    EXPECT_LT(clockSampleWeight(h, 1), clockSampleWeight(0, 1)) << "hops=" << (int)h;
    EXPECT_LE(clockSampleWeight(h, 1), clockSampleWeight(h - 1, 1)) << "hops=" << (int)h;
    EXPECT_GT(clockSampleWeight(h, 1), 0) << "a distant peer still holds one vote";
  }
}

TEST(ClockHops, ThreeDirectSourcesOutvoteFourDistantOnes) {
  /* Four beats three on a plain count, so this only comes out right because a
     reading from eight hops away is worth a twelfth of a direct one. */
  std::vector<ClockSample> v;
  for (int i = 0; i < 3; i++) v.push_back(direct(0));
  for (int i = 0; i < 4; i++) v.push_back(relayed(100, 8));

  auto est = estimate(v);
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(0, est.offset_s);
  EXPECT_EQ(3, est.n_used);
  EXPECT_EQ(3, est.n_direct);
}

TEST(ClockHops, AReadingFromTooFarAwayIsNotUsedAtAll) {
  auto est = estimate({direct(0), direct(0), direct(0),
                       relayed(9999, CLOCK_MAX_HOPS + 1)});
  ASSERT_TRUE(est.valid);
  EXPECT_EQ(3, est.n_seen) << "the distant reading never entered the population";
  EXPECT_EQ(100, est.agree_pct);
}

// ---- the unset state --------------------------------------------------------

TEST(ClockUnset, TheVolatileClockDefaultCountsAsUnset) {
  // A fresh board with no RTC chip starts on 15 May 2024, which is before 2026.
  EXPECT_TRUE(clockIsUnset(VOLATILE_DEFAULT));
  EXPECT_TRUE(clockIsUnset(CLOCK_SET_EPOCH - 1));
  EXPECT_FALSE(clockIsUnset(CLOCK_SET_EPOCH));
  EXPECT_FALSE(clockIsUnset(SET_CLOCK));
}

TEST(ClockUnset, AnUnsetClockJumpsTheWholeWayWithNoRateLimit) {
  const int32_t leap = (int32_t)(SET_CLOCK - VOLATILE_DEFAULT);   // about 9 years
  auto est = estimate({direct(leap), direct(leap)}, CLOCK_UNSET_MIN_SOURCES);
  ASSERT_TRUE(est.valid);

  ClockContext ctx = restedContext(VOLATILE_DEFAULT);
  ctx.since_move_ms = 0;             // no allowance has been earned at all
  ctx.sent_high_s = VOLATILE_DEFAULT;  // and it has already been on the air

  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_STEP, d.action);
  EXPECT_EQ(leap, d.apply_s) << "the whole way, in one move";
  EXPECT_EQ(SET_CLOCK, clockApply(ctx.now_s, d.apply_s));
  EXPECT_GT((int64_t)d.apply_s, (int64_t)CLOCK_SLEW_MAX_S_PER_HOUR);
}

TEST(ClockUnset, AnUnsetClockStillNeedsTwoSourcesThatAgree) {
  const int32_t leap = (int32_t)(SET_CLOCK - VOLATILE_DEFAULT);
  auto one = estimate({direct(leap)}, CLOCK_UNSET_MIN_SOURCES);
  EXPECT_FALSE(one.valid);
  EXPECT_EQ(CLOCK_HOLD_NO_QUORUM,
            clockDecide(one, restedContext(VOLATILE_DEFAULT)).hold);

  // Two sources that are years apart are not two sources that agree.
  auto split = estimate({direct(leap), direct(leap + 90000)}, CLOCK_UNSET_MIN_SOURCES);
  EXPECT_FALSE(split.valid);
}

TEST(ClockUnset, TheOrdinaryFloorOfThreeStillRefusesTwoSources) {
  const int32_t leap = (int32_t)(SET_CLOCK - VOLATILE_DEFAULT);
  auto est = estimate({direct(leap), direct(leap)}, CLOCK_MIN_SOURCES);
  EXPECT_FALSE(est.valid) << "the lower floor belongs to the unset path only";
}

// ---- the rate limit ---------------------------------------------------------

TEST(ClockRate, TheAllowanceGrowsAtSixtySecondsPerHour) {
  EXPECT_EQ(0,  clockSlewAllowanceS(0));
  EXPECT_EQ(1,  clockSlewAllowanceS(60000));           // one minute
  EXPECT_EQ(5,  clockSlewAllowanceS(300000));          // five minutes
  EXPECT_EQ(30, clockSlewAllowanceS(1800000));         // half an hour
  EXPECT_EQ(60, clockSlewAllowanceS(CLOCK_SLEW_WINDOW_MS));
}

TEST(ClockRate, IdleTimeDoesNotBankABiggerJump) {
  EXPECT_EQ(60, clockSlewAllowanceS(24UL * CLOCK_SLEW_WINDOW_MS));
  EXPECT_EQ(60, clockSlewAllowanceS(0xFFFFFFFFu));

  auto est = estimate({direct(5000), direct(5000), direct(5000)});
  ClockContext ctx = restedContext();
  ctx.since_move_ms = 24UL * CLOCK_SLEW_WINDOW_MS;   // quiet for a whole day
  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_SLEW, d.action);
  EXPECT_EQ(60, d.apply_s);
}

TEST(ClockRate, OneMoveNeverExceedsWhatTheElapsedTimeAllows) {
  auto est = estimate({direct(100000), direct(100000), direct(100000)});
  for (uint32_t ms = 0; ms <= 2 * CLOCK_SLEW_WINDOW_MS; ms += 37000) {
    ClockContext ctx = restedContext();
    ctx.since_move_ms = ms;
    auto d = clockDecide(est, ctx);
    EXPECT_LE(d.apply_s, clockSlewAllowanceS(ms)) << "ms=" << ms;
    EXPECT_LE(d.apply_s, CLOCK_SLEW_MAX_S_PER_HOUR) << "ms=" << ms;
  }
}

TEST(ClockRate, ATenMinuteCorrectionTakesTenHours) {
  // 600s at 60 s/hour. The task the rate limit was written for.
  int32_t error = 600;                 // we are 600s slow
  uint32_t elapsed_ms = 0;
  const uint32_t poll_ms = 300000;     // convergence runs every five minutes
  uint32_t since_move = poll_ms;
  int guard = 0;

  while (error > CLOCK_DEADBAND_S && guard++ < 10000) {
    auto est = estimate({direct(error), direct(error), direct(error)});
    ClockContext ctx = restedContext();
    ctx.since_move_ms = since_move;
    auto d = clockDecide(est, ctx);
    if (d.action == CLOCK_SLEW) { error -= d.apply_s; since_move = 0; }
    elapsed_ms += poll_ms;
    since_move += poll_ms;
  }
  ASSERT_LE(error, CLOCK_DEADBAND_S);
  const double hours = elapsed_ms / 3600000.0;
  EXPECT_GE(hours, 9.9) << "it must not get there faster than the limit allows";
  EXPECT_LE(hours, 11.0) << "and it must actually get there";
}

TEST(ClockRate, TheDeadbandStopsPointlessMovement) {
  for (int32_t off = -CLOCK_DEADBAND_S; off <= CLOCK_DEADBAND_S; off++) {
    auto est = estimate({direct(off), direct(off), direct(off)});
    ASSERT_TRUE(est.valid) << "off=" << off;
    auto d = clockDecide(est, restedContext());
    EXPECT_EQ(CLOCK_HOLD, d.action) << "off=" << off;
    EXPECT_EQ(CLOCK_HOLD_IN_BAND, d.hold) << "off=" << off;
  }
}

// ---- the admin override -----------------------------------------------------

TEST(ClockAdmin, AnAdminSetSuppressesConvergenceForSevenDays) {
  EXPECT_EQ(7UL * 24UL * 3600UL * 1000UL, CLOCK_ADMIN_HOLD_MS);

  auto est = estimate({direct(5000), direct(5000), direct(5000)});
  ClockContext ctx = restedContext();
  ctx.admin_hold_ms = CLOCK_ADMIN_HOLD_MS;

  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_HOLD, d.action);
  EXPECT_EQ(CLOCK_HOLD_ADMIN, d.hold);
  EXPECT_EQ(0, d.apply_s);
}

TEST(ClockAdmin, ConvergenceResumesTheMomentTheHoldRunsOut) {
  auto est = estimate({direct(5000), direct(5000), direct(5000)});
  const uint32_t poll_ms = 300000;
  uint32_t hold = CLOCK_ADMIN_HOLD_MS;
  uint32_t waited_ms = 0;

  for (int i = 0; i < 4000 && hold > 0; i++) {
    ClockContext ctx = restedContext();
    ctx.admin_hold_ms = hold;
    EXPECT_EQ(CLOCK_HOLD_ADMIN, clockDecide(est, ctx).hold) << "waited=" << waited_ms;
    hold = clockReduceHold(hold, poll_ms);
    waited_ms += poll_ms;
  }
  EXPECT_EQ(0u, hold);
  EXPECT_EQ(CLOCK_ADMIN_HOLD_MS, waited_ms) << "seven days, to the poll";

  ClockContext ctx = restedContext();
  ctx.admin_hold_ms = hold;
  EXPECT_EQ(CLOCK_SLEW, clockDecide(est, ctx).action);
}

TEST(ClockAdmin, AHoldDoesNotBlockANodeThatHasNoClockAtAll) {
  /* An unset clock is the one state where waiting is worse than acting. A node
     stuck on 15 May 2024 is invisible, not merely wrong. */
  const int32_t leap = (int32_t)(SET_CLOCK - VOLATILE_DEFAULT);
  auto est = estimate({direct(leap), direct(leap)}, CLOCK_UNSET_MIN_SOURCES);
  ClockContext ctx = restedContext(VOLATILE_DEFAULT);
  ctx.admin_hold_ms = CLOCK_ADMIN_HOLD_MS;
  EXPECT_EQ(CLOCK_STEP, clockDecide(est, ctx).action);
}

// ---- the replay hazard ------------------------------------------------------
//
// MeshCore rejects a packet whose timestamp is not newer than the last one that
// the receiver stored for that sender (BaseChatMesh.cpp against
// ContactInfo::last_advert_timestamp, and the servers against
// ClientInfo::last_timestamp). A node whose outgoing timestamps regress is
// therefore dropped by every peer that already knows it.

TEST(ClockReplay, RTCClockKeepsOutgoingTimestampsMonotonicAcrossABackwardSet) {
  /* mesh::RTCClock::getCurrentTimeUnique already holds a high-water mark. This
     covers the login and request paths, which stamp through it. It is pinned
     here because convergence now moves the clock backwards, and this property
     is what stops that breaking those two paths. */
  class FakeClock : public RTCClock {
    uint32_t _t;
  public:
    explicit FakeClock(uint32_t t) : _t(t) {}
    uint32_t getCurrentTime() override { return _t; }
    void setCurrentTime(uint32_t t) override { _t = t; }
  };

  FakeClock clk(SET_CLOCK);
  uint32_t prev = clk.getCurrentTimeUnique();

  clk.setCurrentTime(SET_CLOCK - 600);          // convergence steps back 10 min
  for (int i = 0; i < 700; i++) {
    uint32_t t = clk.getCurrentTimeUnique();
    EXPECT_GT(t, prev) << "i=" << i;
    prev = t;
    clk.setCurrentTime(clk.getCurrentTime() + 1);
  }
}

TEST(ClockReplay, ABurstSlewIsRefusedImmediatelyAfterATransmission) {
  /* The case the rate limit alone does not cover. A node that sat in the
     deadband banks a full hour of allowance. It then adverts, and one second
     later convergence fires. The 60s the limit permits would put the clock 59s
     below the timestamp that just went on the air. */
  auto est = estimate({direct(-600), direct(-600), direct(-600)});
  ASSERT_TRUE(est.valid);

  ClockContext ctx = restedContext();
  ctx.sent_high_s = SET_CLOCK;            // the advert went out this second
  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_HOLD, d.action);
  EXPECT_EQ(CLOCK_HOLD_REPLAY, d.hold);
  EXPECT_EQ(0, d.apply_s);
}

TEST(ClockReplay, ABackwardMoveIsClampedToTheRoomThatQuietTimeEarned) {
  auto est = estimate({direct(-600), direct(-600), direct(-600)});

  ClockContext ctx = restedContext();
  ctx.sent_high_s = SET_CLOCK - 30;      // the last transmission was 30s ago
  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_SLEW, d.action);
  EXPECT_EQ(-29, d.apply_s) << "back to one second above the mark, and no further";
  EXPECT_GT(clockApply(ctx.now_s, d.apply_s), ctx.sent_high_s);

  ctx.sent_high_s = SET_CLOCK - 3600;    // an hour of quiet earns the full 60
  d = clockDecide(est, ctx);
  EXPECT_EQ(-60, d.apply_s) << "the rate limit binds first once the room is there";
}

TEST(ClockReplay, ANodeThatHasSentNothingMayMoveBackwardsFreely) {
  auto est = estimate({direct(-600), direct(-600), direct(-600)});
  ClockContext ctx = restedContext();
  ctx.sent_high_s = 0;
  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_SLEW, d.action);
  EXPECT_EQ(-60, d.apply_s);
}

TEST(ClockReplay, AForwardMoveIsNeverBlockedByTheFloor) {
  auto est = estimate({direct(600), direct(600), direct(600)});
  ClockContext ctx = restedContext();
  ctx.sent_high_s = SET_CLOCK;
  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_SLEW, d.action);
  EXPECT_EQ(60, d.apply_s);
}

TEST(ClockReplay, OutgoingTimestampsNeverRegressUnderAStreamOfCorrections) {
  /* THE regression test for the hazard. The node runs 600s fast and must come
     back down. That is the 10-minute correction the rate limit costs 10 hours.

     The rate limit on its own already forbids most of the failure: the wall
     clock gains 3600 s in an hour and convergence may remove only 60, so a
     clock that moves ONLY at the limit can never regress an outgoing timestamp.
     What defeats that argument is the BURST. The allowance is a bucket, so a
     node that holds for an hour -- for a quiet neighbourhood, an admin hold, or
     a deadband -- may spend a whole 60s in one move, and 60s is far more than
     the wall clock gains in the second between an advert and the next poll.
     This run therefore holds most rounds and lets the bucket fill, which is
     what puts a full burst next to a fresh transmission.

     Delete the sent_high_s clamp in clockDecide and this test fails: the clock
     drops below a timestamp that is already on the air, and the next advert is
     one that every peer holding a mark for this node throws away. */
  uint32_t clock = SET_CLOCK;
  uint32_t sent_high = 0;
  uint32_t prev_stamp = 0;
  uint32_t since_move = CLOCK_SLEW_WINDOW_MS;
  int32_t  error = -600;                  // seconds to ADD to reach the truth
  uint32_t seed = 12345;
  int moves = 0, adverts = 0, bursts = 0;

  for (int i = 0; i < 20000 && error < -CLOCK_DEADBAND_S; i++) {
    seed = seed * 1103515245u + 12345u;
    uint32_t dt_s = 1 + ((seed >> 16) % 20);      // 1..20s of wall time passes
    clock += dt_s;                                // the wall clock advances
    since_move = clockAdvanceElapsed(since_move, dt_s * 1000);

    /* The neighbourhood answers rarely, which is the ordinary life of an
       indoor repeater. Nothing moves in between, so the bucket is full every
       time an answer does arrive. That is what puts a 60s burst next to a
       transmission that went out seconds ago. */
    bool quorum = (i % 400) == 399;
    ClockSample agree[3]    = { direct(error), direct(error), direct(error) };
    ClockSample disagree[3] = { direct(-500), direct(0), direct(500) };
    auto est = clockEstimate(quorum ? agree : disagree, 3);

    // Converge first, then transmit. That ordering is the adversarial one: a
    // backward move is followed immediately by a timestamp on the air.
    ClockContext ctx;
    ctx.now_s = clock;
    ctx.since_move_ms = since_move;
    ctx.admin_hold_ms = 0;
    ctx.sent_high_s = sent_high;
    auto d = clockDecide(est, ctx);
    if (d.action != CLOCK_HOLD) {
      if (d.apply_s <= -(int32_t)dt_s) bursts++;  // it removed more than the wall gained
      clock = clockApply(clock, d.apply_s);
      error -= d.apply_s;
      since_move = 0;
      moves++;
    }
    if (sent_high != 0) {
      EXPECT_GT(clock, sent_high) << "i=" << i << " the clock fell to or below the mark";
    }

    if (((seed >> 8) & 3) == 0) {                 // a quarter of the rounds advert
      uint32_t stamp = clock;
      EXPECT_GT(stamp, prev_stamp) << "i=" << i << " an outgoing timestamp regressed";
      prev_stamp = stamp;
      if (stamp > sent_high) sent_high = stamp;
      adverts++;
    }
  }
  EXPECT_GT(moves, 10) << "the run must actually exercise the slew path";
  EXPECT_GT(adverts, 100);
  EXPECT_GT(bursts, 5) << "and it must actually reach the dangerous case";
  EXPECT_GE(error, -CLOCK_DEADBAND_S) << "and it must still have converged";
}

// ---- the wrap cases ---------------------------------------------------------

TEST(ClockWrap, ThePollDeltaIsCorrectAcrossTheMillisWrap) {
  EXPECT_EQ(66u, clockPollDelta(50u, 0xFFFFFFF0u));
  EXPECT_EQ(300000u, clockPollDelta(300000u, 0u));
  EXPECT_EQ(1u, clockPollDelta(0u, 0xFFFFFFFFu));
}

TEST(ClockWrap, TheMillisWrapDoesNotBringAnExpiredAdminHoldBackToLife) {
  /* The bug that absolute millis() arithmetic would have. A hold that started
     near millis()==0 and lasts 7 days looks unexpired again the moment the
     counter wraps past 0 at 49.7 days. The hold is a counter that the caller
     reduces by each poll interval, so the wrap cannot reach it. */
  auto est = estimate({direct(5000), direct(5000), direct(5000)});
  uint32_t hold = CLOCK_ADMIN_HOLD_MS;
  uint32_t ms = 0xFFFF0000u;                 // a few minutes before the wrap
  bool released = false;

  for (int i = 0; i < 5000; i++) {           // about 17 days of five-minute polls
    uint32_t next = ms + 300000u;            // wraps through zero on the way
    hold = clockReduceHold(hold, clockPollDelta(next, ms));
    ms = next;

    ClockContext ctx = restedContext();
    ctx.admin_hold_ms = hold;
    auto d = clockDecide(est, ctx);
    if (released) {
      EXPECT_NE(CLOCK_HOLD_ADMIN, d.hold) << "i=" << i << " the hold came back";
    } else if (d.hold != CLOCK_HOLD_ADMIN) {
      released = true;
    }
  }
  EXPECT_TRUE(released);
  EXPECT_EQ(0u, hold);
}

TEST(ClockWrap, TheElapsedCounterSaturatesInsteadOfWrapping) {
  // A wrap here would hand a node a fresh allowance it never earned.
  EXPECT_EQ(0xFFFFFFFFu, clockAdvanceElapsed(0xFFFFFFF0u, 100u));
  EXPECT_EQ(0xFFFFFFFFu, clockAdvanceElapsed(0xFFFFFFFFu, 0xFFFFFFFFu));
  EXPECT_EQ(300000u, clockAdvanceElapsed(0u, 300000u));
  EXPECT_EQ(60, clockSlewAllowanceS(clockAdvanceElapsed(0xFFFFFFF0u, 100u)));
}

TEST(ClockWrap, ACorrectionIsAppliedCorrectlyAcrossThe2106EpochWrap) {
  EXPECT_EQ(1u, clockApply(0xFFFFFFFFu, 2));
  EXPECT_EQ(0xFFFFFFFFu, clockApply(1u, -2));
}

TEST(ClockWrap, AMarkThatSitsAheadOfOurClockBlocksEveryBackwardMove) {
  /* The mark can be in the future of our own clock. An unset node that steps to
     an overshooting consensus adverts from up there, and a person can then set
     the clock back below it. Every backward move is refused until the wall
     clock climbs past the mark again.

     This is also the arithmetic that the 2106 epoch wrap needs, and it is the
     only way to reach it: after the wrap a clock reads a small number, and
     clockIsUnset calls every such value unset, so clockDecide takes the step
     path instead. MeshCore's own 32-bit timestamps end at that wrap in any
     case. */
  auto est = estimate({direct(-600), direct(-600), direct(-600)});
  ClockContext ctx = restedContext();
  ctx.sent_high_s = SET_CLOCK + 500;

  auto d = clockDecide(est, ctx);
  EXPECT_EQ(CLOCK_HOLD, d.action);
  EXPECT_EQ(CLOCK_HOLD_REPLAY, d.hold);
  EXPECT_EQ(0, d.apply_s);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
