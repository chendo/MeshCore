#include <gtest/gtest.h>

#include "helpers/AbstractBridge.h"

// The type code and name used to be #if/#elif chains in MyMesh.cpp and
// CommonCLI.cpp. These stand-ins carry the same values as RS232Bridge and
// ESPNowBridge, which cannot be built for the host (UART / esp_now).
class FakeUartBridge : public AbstractBridge {
public:
  void begin() override {}
  void end() override {}
  bool isRunning() const override { return false; }
  void loop() override {}
  void sendPacket(mesh::Packet*) override {}
  void onPacketReceived(mesh::Packet*) override {}
  uint8_t getTypeCode() const override { return 0x01; }
  const char* getTypeName() const override { return "rs232"; }
};

class FakeEspNowBridge : public FakeUartBridge {
public:
  uint8_t getTypeCode() const override { return 0x03; }
  const char* getTypeName() const override { return "espnow"; }
};

static uint8_t featureByte(AbstractBridge* bridge, bool disabled) {
  uint8_t features = 0;
  if (bridge) features |= bridge->getTypeCode();
  if (disabled) features |= 0x80;
  return features;
}

static const char* bridgeType(AbstractBridge* bridge) {
  return bridge ? bridge->getTypeName() : "none";
}

TEST(BridgeType, dispatchesThroughTheBaseClass) {
  FakeUartBridge uart;
  FakeEspNowBridge espnow;
  AbstractBridge* b = &uart;

  EXPECT_EQ(b->getTypeCode(), 0x01);
  EXPECT_STREQ(b->getTypeName(), "rs232");

  b = &espnow;
  EXPECT_EQ(b->getTypeCode(), 0x03);
  EXPECT_STREQ(b->getTypeName(), "espnow");
}

// A build with no bridge answers `bridge.type` from a null pointer, the way
// CommonCLICallbacks::getBridge() defaults.
TEST(BridgeType, reportsNoneWithoutABridge) {
  FakeUartBridge uart;

  EXPECT_STREQ(bridgeType(nullptr), "none");
  EXPECT_STREQ(bridgeType(&uart), "rs232");
}

// Repeater status reply, byte 8: the type code shares the byte with the
// disabled flag, so it must stay clear of 0x80.
TEST(BridgeType, featureByteKeepsTypeCodeClearOfTheDisabledFlag) {
  FakeUartBridge uart;
  FakeEspNowBridge espnow;

  EXPECT_EQ(featureByte(nullptr, false), 0x00);
  EXPECT_EQ(featureByte(&uart, false), 0x01);
  EXPECT_EQ(featureByte(&espnow, false), 0x03);
  EXPECT_EQ(featureByte(&uart, true), 0x81);
  EXPECT_EQ(featureByte(&espnow, true), 0x83);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
