#include <gtest/gtest.h>
#include <helpers/PowerMonitor.h>

namespace {
// Feed a steady voltage for long enough that the trend window closes.
void feed(PowerMonitor& p, uint32_t& t, uint16_t mv, int samples) {
  for (int i = 0; i < samples; i++) { p.sample(t, mv); t += PowerMonitor::SAMPLE_MS; }
}
}

TEST(Power, NoReadingUntilTheBoardGivesOne) {
  PowerMonitor p;
  EXPECT_FALSE(p.hasReading());
  EXPECT_EQ(-1, p.percent());
  EXPECT_EQ(-1, p.minutesRemaining());
  // 0 means the board has no battery sense and must not be taken as a reading.
  p.sample(1000, 0);
  EXPECT_FALSE(p.hasReading());
}

TEST(Power, PercentSpansTheConfiguredRange) {
  PowerMonitor p;
  uint32_t t = 0;
  p.sample(t, 4200);
  EXPECT_EQ(100, p.percent());

  PowerMonitor q;
  q.sample(0, 3000);
  EXPECT_EQ(0, q.percent());

  PowerMonitor r;
  r.sample(0, 3600);
  EXPECT_EQ(50, r.percent());
}

TEST(Power, PercentIsClampedNotWrapped) {
  PowerMonitor p;
  p.sample(0, 4500);              // freshly off charge, above nominal full
  EXPECT_EQ(100, p.percent());
  PowerMonitor q;
  q.sample(0, 2500);              // below the floor
  EXPECT_EQ(0, q.percent());
}

TEST(Power, ARisingVoltageReadsAsCharging) {
  PowerMonitor p;
  uint32_t t = 1000;
  feed(p, t, 3600, 4);
  EXPECT_FALSE(p.isCharging()) << "flat is not charging";
  // Climb well past the noise threshold, for longer than the trend window.
  for (uint16_t mv = 3600; mv < 3900; mv += 10) { p.sample(t, mv); t += PowerMonitor::SAMPLE_MS; }
  EXPECT_TRUE(p.isCharging());
  EXPECT_EQ(-1, p.minutesRemaining()) << "no runtime estimate while charging";
}

TEST(Power, SmallWobblesAreNotCharging) {
  PowerMonitor p;
  uint32_t t = 1000;
  // A few mV of ADC noise and post-transmit recovery must not read as a charge.
  for (int i = 0; i < 60; i++) {
    p.sample(t, (uint16_t)(3600 + (i % 2 ? 6 : -6)));
    t += PowerMonitor::SAMPLE_MS;
  }
  EXPECT_FALSE(p.isCharging());
}

TEST(Power, RuntimeIsWithheldUntilATrendExists) {
  PowerMonitor p;
  uint32_t t = 1000;
  feed(p, t, 4000, 2);            // well short of the 10 minute window
  EXPECT_EQ(-1, p.minutesRemaining())
      << "an early guess is worse than saying nothing";
}

TEST(Power, ASteadyDrainProducesAFiniteEstimate) {
  PowerMonitor p;
  uint32_t t = 1000;
  // Fall from 4000mV at a constant rate over a couple of hours.
  for (int i = 0; i < 240; i++) {
    p.sample(t, (uint16_t)(4000 - i));
    t += PowerMonitor::SAMPLE_MS;
  }
  int32_t mins = p.minutesRemaining();
  ASSERT_GT(mins, 0) << "a clear discharge must yield a number";
  EXPECT_FALSE(p.isCharging());
  // Above the 3000mV floor with headroom left, the answer must be hours, not
  // minutes, and must not be an absurd extrapolation.
  EXPECT_GT(mins, 60);
  EXPECT_LT(mins, 100 * 60);
}

TEST(Power, AFlatVoltageGivesNoEstimateRatherThanInfinity) {
  PowerMonitor p;
  uint32_t t = 1000;
  feed(p, t, 3800, 120);
  EXPECT_EQ(-1, p.minutesRemaining()) << "a flat pack has no computable end";
}

TEST(Power, AnEmptyPackReportsZeroNotNegative) {
  PowerMonitor p;
  uint32_t t = 1000;
  for (int i = 0; i < 240; i++) {
    p.sample(t, (uint16_t)(3010 > i ? 3010 - i : 2900));
    t += PowerMonitor::SAMPLE_MS;
  }
  int32_t mins = p.minutesRemaining();
  EXPECT_TRUE(mins == 0 || mins == -1) << "never a negative runtime, got " << mins;
}

TEST(Power, SamplesAreRateLimited) {
  PowerMonitor p;
  p.sample(0, 4000);
  // A flood of readings inside one interval must not move the average, or the
  // loop rate would decide the smoothing constant.
  for (int i = 0; i < 500; i++) p.sample(100, 3000);
  EXPECT_EQ(4000, p.millivolts());
}

TEST(Power, AMillisWrapCostsOneSampleNotFortyNineDays) {
  PowerMonitor p;
  p.sample(0xFFFFFF00, 4000);
  p.sample(0x00000100, 3900);     // wrapped; ~512ms later, so still rate-limited
  EXPECT_EQ(4000, p.millivolts());
  p.sample(0x00000100 + PowerMonitor::SAMPLE_MS, 3900);
  EXPECT_LT(p.millivolts(), 4000) << "the monitor must resume after a wrap";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
