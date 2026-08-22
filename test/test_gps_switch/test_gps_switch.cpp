#include <gtest/gtest.h>
#include <string.h>

// The header is a variant header, and the part above its GPS_DIAGNOSTIC guard
// has no Arduino in it. That part is what these tests build.
#include "../../variants/thinknode_m1/GpsDiagnostic.h"

// recover defaults to no_pull, which is the driven pin. Pass recover
// explicitly for an open pin, which keeps the level the pull left on it.
static GpsPinProbe probe(uint8_t no_pull, uint8_t up, uint8_t dn,
                         int recover = -1, int high_count = -1, uint8_t samples = 4) {
  GpsPinProbe p;
  p.no_pull = no_pull;
  p.pull_up = up;
  p.pull_down = dn;
  p.recover = (recover < 0) ? no_pull : (uint8_t) recover;
  p.high_count = (high_count < 0) ? (no_pull ? samples : 0) : (uint8_t) high_count;
  p.samples = samples;
  return p;
}

// An open pin holds whatever the last pull left on it, which is the opposite
// of the level the no-pull samples read.
static GpsPinProbe openProbe(uint8_t no_pull) {
  return probe(no_pull, 1, 0, no_pull ? 0 : 1);
}

static GpsSwitchHistory fresh() {
  GpsSwitchHistory h;
  gps_diag_history_reset(h);
  return h;
}

// The reading that the old probe called "FLOATS" and used to advise an
// internal pull for. The hardware it came from has a working switch, and the
// reading was the same in both positions.
TEST(GpsSwitchFinding, UpOneDownZeroInOnePositionIsNotAConclusion) {
  auto h = fresh();
  for (int i = 0; i < 20; i++) gps_diag_history_add(h, probe(1, 1, 0));

  auto f = gps_diag_switch_finding(h);
  EXPECT_EQ(f, GPS_SW_ONE_LEVEL_DRIVEN);
  EXPECT_FALSE(gps_diag_finding_is_conclusive(f));
  EXPECT_EQ(strstr(gps_diag_finding_text(f), "float"), nullptr);
  EXPECT_NE(strstr(gps_diag_finding_text(f), "not open"), nullptr);
}

// Repeating one position must never turn into a conclusion, however long the
// probe runs.
TEST(GpsSwitchFinding, ManyProbesAtOneLevelStayInconclusive) {
  auto h = fresh();
  for (int i = 0; i < 5000; i++) gps_diag_history_add(h, probe(0, 1, 0));
  EXPECT_FALSE(gps_diag_finding_is_conclusive(gps_diag_switch_finding(h)));
}

TEST(GpsSwitchFinding, NoProbeYetIsNoData) {
  auto h = fresh();
  EXPECT_EQ(gps_diag_switch_finding(h), GPS_SW_NO_DATA);
  EXPECT_FALSE(gps_diag_finding_is_conclusive(GPS_SW_NO_DATA));
}

// Both switch positions read differently with plain INPUT, and each of them
// was steady. That is the one case the probe can conclude on, and the
// conclusion is "change nothing".
TEST(GpsSwitchFinding, BothSteadyPlainLevelsMeanTheSwitchWorks) {
  auto h = fresh();
  gps_diag_history_add(h, probe(1, 1, 0));
  gps_diag_history_add(h, probe(0, 1, 0));

  auto f = gps_diag_switch_finding(h);
  EXPECT_EQ(f, GPS_SW_FOLLOWS_SWITCH);
  EXPECT_TRUE(gps_diag_finding_is_conclusive(f));
  EXPECT_NE(strstr(gps_diag_finding_text(f), "Change nothing"), nullptr);
}

// A genuinely open pin keeps the level the pull left on it.
TEST(GpsSwitchFinding, AnOpenPinInOnePositionReportsOpenAndNotAConclusion) {
  auto h = fresh();
  gps_diag_history_add(h, openProbe(1));

  auto f = gps_diag_switch_finding(h);
  EXPECT_EQ(f, GPS_SW_ONE_LEVEL_OPEN);
  EXPECT_FALSE(gps_diag_finding_is_conclusive(f));
}

// An open pin drifts between the two levels by itself. That must not read as a
// working switch.
TEST(GpsSwitchFinding, AnOpenPinThatDriftsIsNotAWorkingSwitch) {
  auto h = fresh();
  gps_diag_history_add(h, probe(1, 1, 0, 0, 3));   // 3 of 4 samples HIGH
  gps_diag_history_add(h, probe(0, 1, 0, 1, 1));   // 1 of 4 samples HIGH

  auto f = gps_diag_switch_finding(h);
  EXPECT_EQ(f, GPS_SW_BOTH_LEVELS_UNSTEADY);
  EXPECT_FALSE(gps_diag_finding_is_conclusive(f));
  EXPECT_NE(strstr(gps_diag_finding_text(f), "not proof"), nullptr);
}

TEST(GpsSwitchFinding, AStrongDriveInOnePositionIsStillInconclusive) {
  auto h = fresh();
  gps_diag_history_add(h, probe(1, 1, 1));
  EXPECT_EQ(gps_diag_switch_finding(h), GPS_SW_ONE_LEVEL_DRIVEN);
  EXPECT_FALSE(gps_diag_finding_is_conclusive(gps_diag_switch_finding(h)));
}

TEST(GpsSwitchFinding, DisagreeingProbesAtOneLevelReportMixed) {
  auto h = fresh();
  gps_diag_history_add(h, probe(1, 1, 0));
  gps_diag_history_add(h, openProbe(1));
  EXPECT_EQ(gps_diag_switch_finding(h), GPS_SW_ONE_LEVEL_MIXED);
  EXPECT_FALSE(gps_diag_finding_is_conclusive(gps_diag_switch_finding(h)));
}

// One position drives hard and the other drives weakly. Both together still
// mean the switch works.
TEST(GpsSwitchFinding, MixedStrengthAcrossTwoLevelsStillMeansTheSwitchWorks) {
  auto h = fresh();
  gps_diag_history_add(h, probe(0, 0, 0));
  gps_diag_history_add(h, probe(1, 1, 0));
  EXPECT_EQ(gps_diag_switch_finding(h), GPS_SW_FOLLOWS_SWITCH);
}

TEST(GpsSwitchHistoryState, LevelsSeenTracksBothPlainLevels) {
  auto h = fresh();
  EXPECT_EQ(h.levels_seen, 0x00);
  gps_diag_history_add(h, probe(1, 1, 0));
  EXPECT_EQ(h.levels_seen, 0x02);
  gps_diag_history_add(h, probe(0, 1, 0));
  EXPECT_EQ(h.levels_seen, 0x03);
  EXPECT_EQ(h.probes, 2);
}

TEST(GpsSwitchHistoryState, UnsteadySamplesAreRecorded) {
  auto h = fresh();
  gps_diag_history_add(h, probe(1, 1, 0, -1, 3));
  EXPECT_NE(h.unsteady, 0);

  auto steady = fresh();
  gps_diag_history_add(steady, probe(1, 1, 0, -1, 4));
  EXPECT_EQ(steady.unsteady, 0);
}

TEST(GpsSwitchHistoryState, ReadingAgainstThePullIsRecorded) {
  auto h = fresh();
  gps_diag_history_add(h, probe(0, 0, 1));
  EXPECT_EQ(h.against_pull, 1);
}

TEST(GpsProbeRecovery, RecoverySeparatesAWeakDriveFromAnOpenPin) {
  EXPECT_TRUE(gps_diag_probe_recovered(probe(1, 1, 0)));
  EXPECT_FALSE(gps_diag_probe_recovered(openProbe(1)));
  EXPECT_TRUE(gps_diag_probe_recovered(probe(0, 1, 0)));
  EXPECT_FALSE(gps_diag_probe_recovered(openProbe(0)));

  EXPECT_NE(strstr(gps_diag_recovery_observation(probe(1, 1, 0)), "something drives it"),
            nullptr);
  EXPECT_NE(strstr(gps_diag_recovery_observation(openProbe(1)), "nothing drives it"), nullptr);
}

// The whole point of the rewrite: an internal pull must never be advised.
TEST(GpsSwitchAdvice, NoTextAdvisesAnInternalPull) {
  const GpsSwitchFinding all[] = {GPS_SW_NO_DATA, GPS_SW_ONE_LEVEL_DRIVEN,
                                  GPS_SW_ONE_LEVEL_OPEN, GPS_SW_ONE_LEVEL_MIXED,
                                  GPS_SW_BOTH_LEVELS_UNSTEADY, GPS_SW_FOLLOWS_SWITCH};
  for (auto f : all) {
    for (const char* s : {gps_diag_finding_text(f), gps_diag_finding_next_step(f)}) {
      EXPECT_EQ(strstr(s, "INPUT_PULLUP"), nullptr) << s;
      EXPECT_EQ(strstr(s, "INPUT_PULLDOWN"), nullptr) << s;
    }
  }

  const GpsPinProbe probes[] = {probe(1, 1, 0), probe(1, 1, 1), probe(0, 0, 0),
                                probe(0, 0, 1), openProbe(1)};
  for (const auto& p : probes) {
    for (const char* s : {gps_diag_probe_observation(p), gps_diag_recovery_observation(p)}) {
      EXPECT_EQ(strstr(s, "INPUT_PULLUP"), nullptr) << s;
      EXPECT_EQ(strstr(s, "INPUT_PULLDOWN"), nullptr) << s;
    }
  }
}

TEST(GpsSwitchAdvice, ThePullRuleFiresWhenAnInternalPullOverridesThePin) {
  auto h = fresh();
  gps_diag_history_add(h, probe(1, 1, 0));
  EXPECT_TRUE(gps_diag_pull_would_override(h));

  auto held = fresh();
  gps_diag_history_add(held, probe(1, 1, 1));
  EXPECT_FALSE(gps_diag_pull_would_override(held));
}

TEST(GpsProbeObservation, EachDriveStrengthGetsItsOwnText) {
  EXPECT_NE(strstr(gps_diag_probe_observation(probe(1, 1, 0)), "followed both"), nullptr);
  EXPECT_NE(strstr(gps_diag_probe_observation(probe(1, 1, 1)), "stayed HIGH"), nullptr);
  EXPECT_NE(strstr(gps_diag_probe_observation(probe(0, 0, 0)), "stayed LOW"), nullptr);
  EXPECT_NE(strstr(gps_diag_probe_observation(probe(0, 0, 1)), "against each"), nullptr);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
