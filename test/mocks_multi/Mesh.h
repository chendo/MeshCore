#pragma once
// Mock of the mesh::Radio interface that SharedRadio funnels through.
#include <Arduino.h>
#include <MeshCore.h>

namespace mesh {
class Radio {
public:
  virtual ~Radio() {}
  virtual void begin() {}
  virtual int recvRaw(uint8_t* bytes, int sz) = 0;
  virtual bool startSendRaw(const uint8_t* bytes, int len) = 0;
  virtual bool isSendComplete() = 0;
  virtual void onSendFinished() = 0;
  virtual bool isInRecvMode() const { return true; }
  virtual bool isReceiving() { return false; }
  virtual uint32_t getEstAirtimeFor(int len) { return len; }
  virtual float packetScore(float snr, int len) { return 0; }
  virtual int getNoiseFloor() const { return -120; }
  virtual void triggerNoiseFloorCalibrate(int threshold) {}
  virtual void setCADEnabled(bool enable) {}
  virtual void resetAGC() {}
  virtual void loop() {}
  virtual float getLastRSSI() const { return -100; }
  virtual float getLastSNR() const { return 0; }
};
}
