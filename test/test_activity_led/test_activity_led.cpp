#include <gtest/gtest.h>
#include <vector>
#include <helpers/ActivityLed.h>

namespace {

const int8_t TX = 13, RX = 36;

// Records the logical duty per pin instead of writing hardware.
class FakeLed : public ActivityLed {
public:
  struct W { int8_t pin; uint8_t duty; };
  std::vector<W> writes;
protected:
  void writePin(int8_t pin, uint8_t duty) override { writes.push_back({pin, duty}); }
public:
  int countFor(int8_t pin) const {
    int n = 0;
    for (auto& w : writes) if (w.pin == pin) n++;
    return n;
  }
};

}  // namespace

TEST(ActivityLed, IdleIsDarkBlueAndLitRed) {
  FakeLed l;
  l.begin(TX, RX, true);
  EXPECT_EQ(0, l.txLevel()) << "blue is dark with no traffic and no BLE";
  EXPECT_EQ(ActivityLed::FULL_DUTY, l.rxLevel()) << "red is lit when idle";
}

TEST(ActivityLed, TransmitPulsesBlueThenReleasesIt) {
  FakeLed l;
  l.begin(TX, RX, true);
  l.notifyTx(1000);
  l.loop(1000);
  EXPECT_EQ(ActivityLed::FULL_DUTY, l.txLevel());
  l.loop(1000 + ActivityLed::TX_PULSE_MS - 1);
  EXPECT_EQ(ActivityLed::FULL_DUTY, l.txLevel()) << "still inside the pulse";
  l.loop(1000 + ActivityLed::TX_PULSE_MS);
  EXPECT_EQ(0, l.txLevel()) << "pulse over, back to idle";
}

TEST(ActivityLed, ReceiveBlanksRedThenRelightsIt) {
  FakeLed l;
  l.begin(TX, RX, true);
  l.notifyRx(5000);
  l.loop(5000);
  EXPECT_EQ(0, l.rxLevel()) << "a receive blanks the lit LED";
  l.loop(5000 + ActivityLed::RX_BLANK_MS);
  EXPECT_EQ(ActivityLed::FULL_DUTY, l.rxLevel()) << "and it comes straight back on";
}

TEST(ActivityLed, BleConnectionHoldsBlueDimNotOff) {
  FakeLed l;
  l.begin(TX, RX, true);
  l.setBleConnected(true);
  l.loop(100);
  EXPECT_EQ(ActivityLed::DIM_DUTY, l.txLevel());
  EXPECT_LT(ActivityLed::DIM_DUTY, ActivityLed::FULL_DUTY) << "dim must be distinguishable";

  // A transmit still has to be visible against the dim backlight...
  l.notifyTx(100);
  l.loop(100);
  EXPECT_EQ(ActivityLed::FULL_DUTY, l.txLevel());
  // ...and must fall back to dim, not to dark, while the link is still up.
  l.loop(100 + ActivityLed::TX_PULSE_MS);
  EXPECT_EQ(ActivityLed::DIM_DUTY, l.txLevel());

  l.setBleConnected(false);
  l.loop(500);
  EXPECT_EQ(0, l.txLevel());
}

TEST(ActivityLed, AnUnchangedLevelIsNeverRewritten) {
  FakeLed l;
  l.begin(TX, RX, true);
  int before = l.countFor(TX);
  for (uint32_t t = 0; t < 100; t++) l.loop(t);
  EXPECT_EQ(before, l.countFor(TX))
      << "a steady LED must not be re-driven every loop; on the nRF52 that would"
         " re-arm a PWM channel thousands of times a second";
}

TEST(ActivityLed, AMillisWrapEndsThePulseInsteadOfHoldingIt) {
  FakeLed l;
  l.begin(TX, RX, true);
  // Stamp a deadline that wraps past the top of the 32-bit counter.
  l.notifyTx(0xFFFFFFF0);
  l.loop(0xFFFFFFF0);
  EXPECT_EQ(ActivityLed::FULL_DUTY, l.txLevel());
  l.loop(0x00000020);   // ~48ms later in real time, having wrapped
  EXPECT_EQ(0, l.txLevel()) << "unsigned wrap must not hold the LED for 49 days";
}

TEST(ActivityLed, ActivePolarityIsAppliedOnceAtTheWrite) {
  // Logical levels stay the same; only the value put on the pin flips. The
  // recorded duty is the logical one, so callers never reason about polarity.
  FakeLed hi, lo;
  hi.begin(TX, RX, true);
  lo.begin(TX, RX, false);
  EXPECT_EQ(hi.txLevel(), lo.txLevel());
  EXPECT_EQ(hi.rxLevel(), lo.rxLevel());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
