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

  /* Set both LEDs off at the start. _lvl_* is 0xFF, so the code does not skip
     these writes. */
  write(_pin_blue, _lvl_blue, 0);
  write(_pin_green, _lvl_green, 0);

  /* Delay the first heartbeat, so the boot does not flash immediately. A flash
     during the init looks the same as a fault indication. */
  _last_hb = millis();
}

void StatusLed::write(uint8_t pin, uint8_t &cache, uint8_t level) {
  if (cache == level) return;
  cache = level;
  analogWrite(pin, _active_high ? level : (uint8_t)(255 - level));
}

void StatusLed::flash(unsigned long &until, uint8_t &level, uint8_t want_level, uint16_t ms) {
  unsigned long now = millis();
  bool still_lit = (long)(now - until) < 0;

  /* A receive that overlaps a transmit must never hide that transmit.
     Therefore the brightness only increases while an LED is still lit. */
  level = (still_lit && level > want_level) ? level : want_level;

  unsigned long want_until = now + ms;
  if (!still_lit || (long)(want_until - until) > 0) until = want_until;
}

void StatusLed::notifyLoraTx() {
  if (_enabled) flash(_green_until, _green_level, BRIGHT_LEVEL, BRIGHT_MS);
}

void StatusLed::notifyLoraRx() {
  if (_enabled) flash(_green_until, _green_level, DIM_LEVEL, DIM_MS);
}

void StatusLed::notifyBleTx() {
  if (_enabled) flash(_blue_until, _blue_level, BRIGHT_LEVEL, BRIGHT_MS);
}

void StatusLed::notifyBleRx() {
  if (_enabled) flash(_blue_until, _blue_level, DIM_LEVEL, DIM_MS);
}

void StatusLed::loraTx() { if (instance) instance->notifyLoraTx(); }
void StatusLed::loraRx() { if (instance) instance->notifyLoraRx(); }
void StatusLed::bleTx()  { if (instance) instance->notifyBleTx(); }
void StatusLed::bleRx()  { if (instance) instance->notifyBleRx(); }

void StatusLed::loop() {
  if (!_enabled) return;

  unsigned long now = millis();

  if (now - _last_hb >= HEARTBEAT_PERIOD_MS) {
    _last_hb = now;
    _hb_until = now + HEARTBEAT_ON_MS;
  }

  /* Use a signed comparison, so the deadlines stay correct when millis()
     wraps. */
  bool hb = (long)(now - _hb_until) < 0;

  uint8_t blue = ((long)(now - _blue_until) < 0) ? _blue_level : 0;
  uint8_t green = ((long)(now - _green_until) < 0) ? _green_level : 0;

  /* The heartbeat sets a minimum level. It does not replace the level. True
     activity during the tick still shows at its own brightness. */
  if (hb) {
    if (blue < DIM_LEVEL) blue = DIM_LEVEL;
    if (green < DIM_LEVEL) green = DIM_LEVEL;
  }

  write(_pin_blue, _lvl_blue, blue);
  write(_pin_green, _lvl_green, green);
}
