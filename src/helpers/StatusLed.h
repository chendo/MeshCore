#pragma once

#include <stdint.h>

/**
 * @brief  Two-LED activity and status indicator.
 *
 * Built for boards that have two single-colour LEDs rather than an RGB one --
 * the RAK3401 has exactly green (P0.35) and blue (P0.36), and no red. Meaning is
 * therefore carried by WHICH LEDs light and in what pattern, not by mixing a
 * colour:
 *
 *   transmit    blue only, brief flash
 *   receive     green only, brief flash
 *   heartbeat   BOTH together every 5s -- the only event that lights both, so
 *               it stays legible even during heavy traffic
 *   charging    both held at a low glow between events (USB power present)
 *
 * Brightness is real hardware PWM (analogWrite), so the glow does not depend on
 * how often loop() gets called. Nothing else in this firmware uses the nRF52
 * PWM peripherals.
 */
class StatusLed {
public:
  /**
   * @param pin_blue   GPIO for the blue LED
   * @param pin_green  GPIO for the green LED
   * @param on_state   1 if the LEDs are active-high, 0 if active-low
   */
  void begin(uint8_t pin_blue, uint8_t pin_green, uint8_t on_state = 1);

  /** Flash blue. Safe to call at any rate; repeat calls just extend the flash. */
  void notifyTx();

  /** Flash green. */
  void notifyRx();

  /**
   * @param charging  true while the board is on external (USB) power. Note the
   *                  RAK3401 exposes VBUS presence, not battery charge current,
   *                  so this means "powered", not strictly "charging".
   */
  void loop(bool charging);

  bool isEnabled() const { return _enabled; }

  /* Static shims so code that has no reference to the instance -- the mesh log
     hooks, the BLE bridge -- can still flash it. No-ops when unconfigured. */
  static void txBlink();
  static void rxBlink();

private:
  static const uint16_t ACTIVITY_MS = 40;
  static const uint16_t HEARTBEAT_ON_MS = 60;
  static const uint32_t HEARTBEAT_PERIOD_MS = 5000;

  /* Low enough to read as "idle but powered" rather than competing with a
     flash, high enough to see across a room. */
  static const uint8_t GLOW_LEVEL = 6;
  static const uint8_t FULL_LEVEL = 255;

  void write(uint8_t pin, uint8_t &cache, uint8_t level);

  bool _enabled = false;
  bool _active_high = true;
  uint8_t _pin_blue = 0, _pin_green = 0;

  /* Last level written per pin, so a steady state does not re-run analogWrite
     on every loop iteration. 0xFF means "nothing written yet". */
  uint8_t _lvl_blue = 0xFF, _lvl_green = 0xFF;

  unsigned long _blue_until = 0, _green_until = 0;
  unsigned long _hb_until = 0, _last_hb = 0;
};
