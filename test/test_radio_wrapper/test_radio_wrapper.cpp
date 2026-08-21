#include <gtest/gtest.h>

#include "helpers/radiolib/RadioLibWrappers.h"

// Minimal concrete wrapper: RadioLibWrapper leaves three hooks abstract, and
// none of them are involved in what these tests cover.
class TestWrapper : public RadioLibWrapper {
public:
  TestWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) {}
  bool isReceivingPacket() override { return false; }
  float getCurrentRSSI() override { return -120.0f; }
  void setParams(float, float, uint8_t, uint8_t) override {}
};

class TestBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 4200; }
  const char* getManufacturerName() const override { return "test"; }
  void reboot() override {}
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

TEST(CodingRateDecode, DefinedFieldValuesBecomeTheDenominator) {
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(1), 5);
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(2), 6);
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(3), 7);
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(4), 8);
}

// 0 is "no frame decoded yet"; 5..7 are reserved in the LoRa header. Reporting
// any of them as a coding rate would be a guess, and callers cannot tell a
// guess from a reading.
TEST(CodingRateDecode, EverythingElseIsUnknown) {
  EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate(0), 0);
  for (int raw = 5; raw <= 255; raw++) {
    EXPECT_EQ(RadioLibWrapper::decodeHeaderCodingRate((uint8_t)raw), 0) << "raw=" << raw;
  }
}

// A radio that cannot read the field must say so rather than assume our own
// coding rate: in explicit-header mode the value belongs to the sender.
TEST(CodingRateDecode, TheBaseRadioReportsUnknown) {
  PhysicalLayer radio;
  TestBoard board;
  TestWrapper w(radio, board);
  EXPECT_EQ(w.getLastRxCodingRate(), 0);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
