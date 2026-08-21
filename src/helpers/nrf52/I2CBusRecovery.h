#pragma once

#include <Arduino.h>

/**
 * @brief  Frees an I2C bus that a slave holds with SDA low.
 *
 * This code exists because the TWIM driver of the Adafruit core polls for
 * completion with bare spins. Two of those spins have no exit at all:
 *
 *   Wire_nRF52.cpp:175   while(!_p_twim->EVENTS_STOPPED);
 *   Wire_nRF52.cpp:241   while(!_p_twim->EVENTS_STOPPED);
 *
 * The RXSTARTED/LASTRX spins above them do exit on EVENTS_ERROR. These two do
 * not. If a slave is in the middle of a transaction and holds SDA low,
 * TASKS_STOP never completes, and the CPU spins there forever. On a repeater
 * that is a full blackout, with no BLE, no LoRa and no CLI. The code reaches
 * this point from setup(), through the SSD1306 and the RTC probe, before
 * anything exists to find the fault.
 *
 * The usual cause is our exact failure mode. The node resets in the middle of
 * a read, because of a brownout, the watchdog or the reset button. The master
 * disappears in the middle of a byte. The slave then waits for the clocks that
 * it needs to shift the data out. It holds SDA low for an unlimited time, and
 * the next boot stops on the first transfer.
 *
 * The remedy is the standard one. Before the code gives the pins to TWIM, it
 * drives SCL by hand and supplies the clocks the slave waits for. Then it sends
 * a STOP. The Wire.begin() of the core cannot do this, because at that time the
 * peripheral owns the pins.
 *
 * A patch to the core would be the direct fix. But
 * framework-arduinoadafruitnrf52 is a managed dependency, and PlatformIO
 * restores it at each reinstall. Therefore the fix must be in code that we own.
 */
namespace I2CBusRecovery {

/**
 * Send clocks to a slave that holds the bus. Call this BEFORE Wire.begin() and
 * before the begin() of any driver.
 * Returns true if the bus was already free, or if this function freed it.
 */
inline bool recover(uint8_t sda_pin, uint8_t scl_pin) {
  /* Set both pins to plain GPIO with pullups. An idle I2C bus holds both lines
     high. */
  pinMode(sda_pin, INPUT_PULLUP);
  pinMode(scl_pin, INPUT_PULLUP);
  delayMicroseconds(10);

  if (digitalRead(sda_pin) == HIGH) return true;   // the bus is already free

  /* Send a maximum of 9 clocks. A slave can be at most 8 data bits plus an ACK
     into a byte. Therefore 9 clocks always carry it past the end, and it
     releases SDA. The code is open-drain throughout. It drives the line LOW,
     and it releases the line to let the pullup make the high. It never drives
     the line high against a slave that pulls it down. */
  for (int i = 0; i < 9 && digitalRead(sda_pin) == LOW; i++) {
    pinMode(scl_pin, OUTPUT);
    digitalWrite(scl_pin, LOW);
    delayMicroseconds(5);
    pinMode(scl_pin, INPUT_PULLUP);
    delayMicroseconds(5);
  }

  bool freed = (digitalRead(sda_pin) == HIGH);

  /* STOP condition: SDA goes low to high while SCL is high. The state machine
     of the slave then returns to idle. Without this, the slave continues to
     hold a transfer open. */
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
