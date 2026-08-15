#pragma once

#include <stdint.h>

/**
 * @brief  Two-LED radio activity indicator.
 *
 * The RAK3401 has exactly two LEDs, green (P0.35) and blue (P0.36), and no red.
 * Rather than try to mix a colour it cannot make, this uses the two axes it does
 * have: COLOUR says which radio, BRIGHTNESS says which direction.
 *
 *              dim = receive        bright = transmit
 *   blue       BLE bridge RX        BLE bridge TX
 *   green      LoRa RX              LoRa TX
 *
 *   heartbeat  both dim together, every 5s
 *
 * So a node quietly listening to LoRa ticks dim green; one relaying a flood
 * flashes bright green; and a bridged packet lights blue alongside it, because
 * bridging happens on the back of a LoRa transmit. Nothing is lit when idle
 * except the heartbeat.
 *
 * Brightness is real hardware PWM (analogWrite), so levels do not depend on how
 * often loop() gets called. Nothing else in this firmware uses the nRF52 PWM
 * peripherals.
 */
class StatusLed {
public:
  /**
   * @param pin_blue   GPIO for the blue LED
   * @param pin_green  GPIO for the green LED
   * @param on_state   1 if the LEDs are active-high, 0 if active-low
   */
  void begin(uint8_t pin_blue, uint8_t pin_green, uint8_t on_state = 1);

  void notifyLoraTx();   // bright green
  void notifyLoraRx();   // dim green
  void notifyBleTx();    // bright blue
  void notifyBleRx();    // dim blue

  /** Drive the LEDs. Call every main-loop iteration. */
  void loop();

  bool isEnabled() const { return _enabled; }

  /* Static shims so code with no reference to the instance -- the mesh log
     hooks, the BLE bridge -- can flash it. No-ops when unconfigured. */
  static void loraTx();
  static void loraRx();
  static void bleTx();
  static void bleRx();

private:
  /* A dim flash is much harder to notice than a bright one, so it is held a
     little longer to even out how visible the two are. */
  static const uint16_t BRIGHT_MS = 40;
  static const uint16_t DIM_MS = 70;

  static const uint16_t HEARTBEAT_ON_MS = 70;
  static const uint32_t HEARTBEAT_PERIOD_MS = 5000;

  /* Low enough to read clearly as "not a transmit", high enough to see across
     a room. Raise DIM_LEVEL if the receive flashes are too subtle. */
  static const uint8_t DIM_LEVEL = 24;
  static const uint8_t BRIGHT_LEVEL = 255;

  void flash(unsigned long &until, uint8_t &level, uint8_t want_level, uint16_t ms);
  void write(uint8_t pin, uint8_t &cache, uint8_t level);

  bool _enabled = false;
  bool _active_high = true;
  uint8_t _pin_blue = 0, _pin_green = 0;

  /* Last level written per pin, so a steady state does not re-run analogWrite
     on every loop iteration. 0xFF means "nothing written yet". */
  uint8_t _lvl_blue = 0xFF, _lvl_green = 0xFF;

  /* Deadline and the level to hold until it, per LED. */
  unsigned long _blue_until = 0, _green_until = 0;
  uint8_t _blue_level = 0, _green_level = 0;

  unsigned long _hb_until = 0, _last_hb = 0;
};
