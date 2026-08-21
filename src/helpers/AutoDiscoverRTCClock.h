#pragma once

#include <Mesh.h>
#include <Arduino.h>
#include <Wire.h>

class AutoDiscoverRTCClock : public mesh::RTCClock {
  mesh::RTCClock* _fallback;

  bool i2c_probe(TwoWire& wire, uint8_t addr);
public:
  AutoDiscoverRTCClock(mesh::RTCClock& fallback) : _fallback(&fallback) { }

  void begin(TwoWire& wire);
  uint32_t getCurrentTime() override;
  void setCurrentTime(uint32_t time) override;

  // Which clock actually answers getCurrentTime(): the name of the I2C chip
  // that was found, or "fallback" when none was and the board's own clock is
  // standing in. This matters to anything measuring drift — on the fallback
  // path there is no second oscillator to measure, so the "error" is whatever
  // the platform clock says about itself, which on ESP32 is nothing at all
  // (SNTP disciplines the very clock being read).
  static const char* deviceName();
  static bool hasHardwareRTC();

  // Did the chip's oscillator stop since it was last set? Both the PCF8563 and
  // the DS3231 raise a flag for this (VL / OSF), and it is the difference
  // between "the crystal is a bit fast" and "the timekeeping was interrupted
  // and whatever it now reads is meaningless". Nothing here has ever looked at
  // it, so a stopped oscillator has been indistinguishable from drift.
  //
  // Tri-state: 1 = stopped, 0 = ran continuously, -1 = this chip cannot say.
  // Reading it is NOT free of side effects on every part, so callers should
  // sample it once at boot and latch the answer.
  static int oscillatorStopped();

  void tick() override {
    _fallback->tick();   // is typically VolatileRTCClock, which now needs tick()
  }
};
