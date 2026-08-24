#pragma once

#include <stdint.h>

/**
 * \brief  Two LEDs reporting radio traffic, without blocking for a moment.
 *
 * TX LED: dark when idle, or held DIM while a BLE connection is up, and pulsed
 *         to full brightness for each transmission.
 * RX LED: lit when idle, and blinked OFF for each reception. Inverted on
 *         purpose -- a receive is a momentary event on a LED that is otherwise
 *         steady, which reads very differently from a dark LED that flickers,
 *         and it lets one LED say "powered" and "receiving" at once.
 *
 * No delay() anywhere: notifyTx()/notifyRx() only stamp a deadline, and loop()
 * returns the LED to its idle level once that deadline passes. Both are safe to
 * call from the radio path, which is why they do no I/O beyond one write.
 *
 * The class knows nothing about radios, BLE or Arduino types beyond the two pin
 * numbers, so it is testable on the host with the writes recorded.
 */
class ActivityLed {
public:
  // Long enough to see, short enough not to smear at the packet rates a busy
  // repeater sees. A LoRa frame at SF9 already occupies the air far longer.
  static const uint32_t TX_PULSE_MS = 40;
  static const uint32_t RX_BLANK_MS = 60;
  // Visible in a dark room, unmistakably not the TX pulse next to it.
  static const uint8_t  DIM_DUTY = 12;
  static const uint8_t  FULL_DUTY = 255;

  ActivityLed() : _tx_pin(-1), _rx_pin(-1), _on_high(true), _ble(false),
                  _tx_until(0), _rx_until(0), _tx_level(-1), _rx_level(-1) { }

  /** \param on_high  true when driving the pin HIGH lights the LED. */
  void begin(int8_t tx_pin, int8_t rx_pin, bool on_high);

  void notifyTx(uint32_t now_ms) { _tx_until = now_ms + TX_PULSE_MS; }
  void notifyRx(uint32_t now_ms) { _rx_until = now_ms + RX_BLANK_MS; }
  void setBleConnected(bool up) { _ble = up; }

  void loop(uint32_t now_ms);

  // for tests
  int txLevel() const { return _tx_level; }
  int rxLevel() const { return _rx_level; }

protected:
  /** Write a duty of 0..255 in LOGICAL terms: 0 is dark, 255 is full. The
   *  override for the pin's polarity happens here, once, so no caller has to
   *  think about it. Virtual so a host test can record instead of writing. */
  virtual void writePin(int8_t pin, uint8_t duty);

private:
  int8_t   _tx_pin, _rx_pin;
  bool     _on_high;
  bool     _ble;
  uint32_t _tx_until, _rx_until;
  int      _tx_level, _rx_level;   // last written, -1 = never

  void apply(int8_t pin, int& cache, uint8_t duty) {
    if (pin < 0 || cache == (int)duty) return;   // never rewrite an unchanged level
    cache = (int)duty;
    writePin(pin, duty);
  }
};
