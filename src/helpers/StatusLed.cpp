#include "StatusLed.h"

#include <Arduino.h>

static StatusLed *instance = nullptr;

void StatusLed::begin(uint8_t pin_blue, uint8_t pin_green, uint8_t on_state) {
  _pin_blue = pin_blue;
  _pin_green = pin_green;
  _active_high = (on_state != 0);

  pinMode(_pin_blue, OUTPUT);
  pinMode(_pin_green, OUTPUT);

  _enabled = true;
  instance = this;

  /* Both off to start. _lvl_* is 0xFF so these writes are not skipped. */
  write(_pin_blue, _lvl_blue, 0);
  write(_pin_green, _lvl_green, 0);

  /* Offset the first heartbeat so boot does not flash immediately -- a flash
     during init would be indistinguishable from a fault indication. */
  _last_hb = millis();
}

void StatusLed::write(uint8_t pin, uint8_t &cache, uint8_t level) {
  if (cache == level) return;
  cache = level;
  analogWrite(pin, _active_high ? level : (uint8_t)(255 - level));
}

void StatusLed::notifyTx() {
  if (!_enabled) return;
  _blue_until = millis() + ACTIVITY_MS;
}

void StatusLed::notifyRx() {
  if (!_enabled) return;
  _green_until = millis() + ACTIVITY_MS;
}

void StatusLed::txBlink() {
  if (instance) instance->notifyTx();
}

void StatusLed::rxBlink() {
  if (instance) instance->notifyRx();
}

void StatusLed::loop(bool charging) {
  if (!_enabled) return;

  unsigned long now = millis();

  if (now - _last_hb >= HEARTBEAT_PERIOD_MS) {
    _last_hb = now;
    _hb_until = now + HEARTBEAT_ON_MS;
  }

  /* Signed comparison so the deadlines survive millis() wrapping. */
  bool hb = (long)(now - _hb_until) < 0;
  bool blue = hb || (long)(now - _blue_until) < 0;
  bool green = hb || (long)(now - _green_until) < 0;

  /* Charging is the resting state, so it only shows through when no event is
     lighting that LED. An event always wins -- activity must stay visible on a
     powered node. */
  uint8_t base = charging ? GLOW_LEVEL : 0;

  write(_pin_blue, _lvl_blue, blue ? FULL_LEVEL : base);
  write(_pin_green, _lvl_green, green ? FULL_LEVEL : base);
}
