#pragma once
// Stand-in for the phone app's TCP transport: tests drive inbound frames and
// inspect what the mux wrote back to the "socket".
#include <helpers/BaseSerialInterface.h>
#include <vector>
class SerialWifiInterface : public BaseSerialInterface {
public:
  bool connected = false;
  std::vector<std::vector<uint8_t>> written;    // frames sent to the app
  std::vector<std::vector<uint8_t>> inbound;    // frames the app sent us
  int port = 0;

  void begin(int p) { port = p; }
  void enable() override {}
  void disable() override {}
  bool isEnabled() const override { return true; }
  bool isConnected() const override { return connected; }
  bool isWriteBusy() const override { return false; }
  size_t writeFrame(const uint8_t src[], size_t len) override {
    written.emplace_back(src, src + len);
    return len;
  }
  size_t checkRecvFrame(uint8_t dest[]) override {
    if (inbound.empty()) return 0;
    auto f = inbound.front();
    inbound.erase(inbound.begin());
    memcpy(dest, f.data(), f.size());
    return f.size();
  }
};
