#pragma once
// This class replaces the TCP transport of the phone app. The tests supply the
// inbound frames. The tests then examine what the mux wrote back to the socket.
#include <helpers/BaseSerialInterface.h>
#include <vector>
class SerialWifiInterface : public BaseSerialInterface {
public:
  bool connected = false;
  std::vector<std::vector<uint8_t>> written;    // the frames that the mux sent to the app
  std::vector<std::vector<uint8_t>> inbound;    // the frames that the app sent to us
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
