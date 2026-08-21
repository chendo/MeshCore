#pragma once

#include <Arduino.h>

/**
 * @brief  Frees an I2C bus left stuck by a slave holding SDA low.
 *
 * Why this exists: the Adafruit core's TWIM driver polls for completion with
 * bare spins, and two of them have no escape at all --
 *
 *   Wire_nRF52.cpp:175   while(!_p_twim->EVENTS_STOPPED);
 *   Wire_nRF52.cpp:241   while(!_p_twim->EVENTS_STOPPED);
 *
 * (the RXSTARTED/LASTRX spins above them at least break on EVENTS_ERROR; these
 * two do not). If a slave is mid-transaction and holding SDA low, TASKS_STOP
 * never completes and the CPU spins there forever. On a repeater that is a
 * total blackout -- no BLE, no LoRa, no CLI -- reached from setup() via the
 * SSD1306 and the RTC probe, before anything exists to notice.
 *
 * The classic cause is exactly our failure mode: the node resets partway
 * through a read (brownout, watchdog, or the reset button), the master
 * disappears mid-byte, and the slave is left waiting for the clocks it needs to
 * finish shifting out. It holds SDA low indefinitely, and the next boot wedges
 * on the first transfer.
 *
 * The remedy is the standard one: before handing the pins to TWIM, drive SCL
 * manually and give the slave the clocks it is waiting for, then a STOP. The
 * core's Wire.begin() cannot do this because by then the peripheral owns the
 * pins.
 *
 * Patching the core would be the direct fix, but framework-arduinoadafruitnrf52
 * is a managed dependency and PlatformIO restores it on reinstall, so the fix
 * has to live in code we own.
 */
namespace I2CBusRecovery {

/**
 * Clock out a stuck slave. Call BEFORE Wire.begin() / any driver begin().
 * Returns true if the bus was already free or was successfully recovered.
 */
inline bool recover(uint8_t sda_pin, uint8_t scl_pin) {
  /* Both as plain GPIO with pullups -- idle I2C is both lines high. */
  pinMode(sda_pin, INPUT_PULLUP);
  pinMode(scl_pin, INPUT_PULLUP);
  delayMicroseconds(10);

  if (digitalRead(sda_pin) == HIGH) return true;   // bus already free, nothing to do

  /* Up to 9 clocks: a slave can be at most 8 data bits plus an ACK into a byte,
     so 9 always carries it past the end and it releases SDA. Open-drain
     throughout -- drive LOW, and release to let the pullup make the high, never
     drive high against a slave that is pulling down. */
  for (int i = 0; i < 9 && digitalRead(sda_pin) == LOW; i++) {
    pinMode(scl_pin, OUTPUT);
    digitalWrite(scl_pin, LOW);
    delayMicroseconds(5);
    pinMode(scl_pin, INPUT_PULLUP);
    delayMicroseconds(5);
  }

  bool freed = (digitalRead(sda_pin) == HIGH);

  /* STOP condition: SDA low->high while SCL is high, so the slave's state
     machine returns to idle rather than believing a transfer is still open. */
  pinMode(sda_pin, OUTPUT);
  digitalWrite(sda_pin, LOW);
  delayMicroseconds(5);
  pinMode(scl_pin, INPUT_PULLUP);
  delayMicroseconds(5);
  pinMode(sda_pin, INPUT_PULLUP);
  delayMicroseconds(5);

  return freed;
}

}
