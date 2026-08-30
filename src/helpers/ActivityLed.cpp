#include "ActivityLed.h"

#ifdef ARDUINO
  #include <Arduino.h>
#endif

/* Out-of-line definitions. The in-class initialisers are only declarations, and
   anything that binds these to a reference -- a test assertion, for one -- needs
   a real object to point at. Cheaper than depending on C++17 inline statics,
   since the firmware builds do not all use it. */
const uint32_t ActivityLed::TX_PULSE_MS;
const uint32_t ActivityLed::RX_BLANK_MS;
const uint8_t  ActivityLed::DIM_DUTY;
const uint8_t  ActivityLed::FULL_DUTY;

void ActivityLed::begin(int8_t tx_pin, int8_t rx_pin, bool on_high) {
  _tx_pin = tx_pin;
  _rx_pin = rx_pin;
  _on_high = on_high;
#ifdef ARDUINO
  if (_tx_pin >= 0) pinMode(_tx_pin, OUTPUT);
  if (_rx_pin >= 0) pinMode(_rx_pin, OUTPUT);
#endif
  _tx_level = _rx_level = -1;    // force the first apply() to write
  loop(0);
}

void ActivityLed::writePin(int8_t pin, uint8_t duty) {
#ifdef ARDUINO
  uint8_t out = _on_high ? duty : (uint8_t)(255 - duty);
  /* Full and dark go out as digital writes. analogWrite at the extremes leaves
     a PWM channel running to hold a constant level, and on the nRF52 there are
     only three of them -- the buzzer and the display backlight want one too. */
  if (duty == 0)             digitalWrite(pin, _on_high ? LOW : HIGH);
  else if (duty == FULL_DUTY) digitalWrite(pin, _on_high ? HIGH : LOW);
  else                        analogWrite(pin, out);
#else
  (void)pin; (void)duty;
#endif
}

void ActivityLed::loop(uint32_t now_ms) {
  /* Signed comparison against the deadline, so a millis() wrap ends a pulse
     early instead of holding the LED for another 49 days. */
  bool tx_active = (int32_t)(_tx_until - now_ms) > 0;
  bool rx_active = (int32_t)(_rx_until - now_ms) > 0;

  apply(_tx_pin, _tx_level, tx_active ? FULL_DUTY : (_ble ? DIM_DUTY : 0));
  // Idle is LIT; a reception blanks it.
  apply(_rx_pin, _rx_level, rx_active ? 0 : FULL_DUTY);
}
