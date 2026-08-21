#pragma once

#include <stdint.h>

/**
 * @brief  Two-LED radio activity indicator.
 *
 * The RAK3401 has exactly two LEDs: green (P0.35) and blue (P0.36). It has no
 * red LED. The board cannot mix a third colour, so this code uses the two axes
 * that it does have. The COLOUR shows which radio. The BRIGHTNESS shows which
 * direction.
 *
 *              dim = receive        bright = transmit
 *   blue       BLE bridge RX        BLE bridge TX
 *   green      LoRa RX              LoRa TX
 *
 *   heartbeat  both LEDs dim together, one time every 5s
 *
 * A node that listens quietly to LoRa gives dim green ticks. A node that
 * relays a flood gives bright green flashes. A bridged packet makes blue at
 * the same time, because the bridge sends its data with a LoRa transmit. When
 * the node is idle, only the heartbeat is lit.
 *
 * The brightness uses true hardware PWM (analogWrite). Therefore the levels do
 * not depend on how frequently the code calls loop(). Nothing else in this
 * firmware uses the nRF52 PWM peripherals.
 */
class StatusLed {
public:
  /**
   * @param pin_blue   GPIO for the blue LED
   * @param pin_green  GPIO for the green LED
   * @param on_state   1 if the LEDs are active-high, 0 if they are active-low
   */
  void begin(uint8_t pin_blue, uint8_t pin_green, uint8_t on_state = 1);

  void notifyLoraTx();   // bright green
  void notifyLoraRx();   // dim green
  void notifyBleTx();    // bright blue
  void notifyBleRx();    // dim blue

  /** Drive the LEDs. Call this on every main-loop iteration. */
  void loop();

  bool isEnabled() const { return _enabled; }

  /* Static shims. They let code that has no reference to the instance flash
     the LEDs. Examples are the mesh log hooks and the BLE bridge. They do
     nothing while the LEDs are not configured. */
  static void loraTx();
  static void loraRx();
  static void bleTx();
  static void bleRx();

private:
  /* A dim flash is much more difficult to see than a bright one. Therefore the
     code holds a dim flash for a longer time. The two flashes are then equally
     easy to see. */
  static const uint16_t BRIGHT_MS = 40;
  static const uint16_t DIM_MS = 70;

  static const uint16_t HEARTBEAT_ON_MS = 70;
  static const uint32_t HEARTBEAT_PERIOD_MS = 5000;

  /* This level is low enough to read clearly as "not a transmit". It is also
     high enough to see across a room. Increase DIM_LEVEL if the receive
     flashes are too difficult to see. */
  static const uint8_t DIM_LEVEL = 24;
  static const uint8_t BRIGHT_LEVEL = 255;

  void flash(unsigned long &until, uint8_t &level, uint8_t want_level, uint16_t ms);
  void write(uint8_t pin, uint8_t &cache, uint8_t level);

  bool _enabled = false;
  bool _active_high = true;
  uint8_t _pin_blue = 0, _pin_green = 0;

  /* The last level written to each pin. A steady state then does not run
     analogWrite again on every loop iteration. 0xFF means that the code has
     written nothing to that pin. */
  uint8_t _lvl_blue = 0xFF, _lvl_green = 0xFF;

  /* For each LED: the deadline, and the level to hold until that deadline. */
  unsigned long _blue_until = 0, _green_until = 0;
  uint8_t _blue_level = 0, _green_level = 0;

  unsigned long _hb_until = 0, _last_hb = 0;
};
