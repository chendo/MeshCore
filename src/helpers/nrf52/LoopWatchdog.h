#pragma once

#include <stdint.h>

/**
 * @brief  Resets the node if the main loop stops advancing.
 *
 * Deliberately NOT the hardware WDT. The nRF52 watchdog cannot be stopped once
 * started and survives a soft reset, so it keeps counting after the app jumps
 * to the bootloader -- and a DFU over BLE takes around 200 seconds. Nothing in
 * the Arduino core feeds it, and the bootloader ships as a prebuilt binary, so
 * whether IT feeds one is unverifiable from here. Enabling the hardware
 * watchdog would therefore risk resetting a node mid-update, on hardware whose
 * only remote route in is that update.
 *
 * This runs entirely inside the application: feed() from the main loop, check()
 * from BLE event context, which preempts the loop and fires constantly (17 to
 * 126 advert reports a second in a populated environment). If the loop stops
 * advancing while BLE still runs, the node resets. The two liveness checks are
 * mutual -- the loop watches the BLE receive path, BLE watches the loop -- so a
 * single wedged subsystem is caught by the other.
 *
 * What it cannot catch is a fault that stops both, which is exactly the case
 * the hardware watchdog exists for. That trade is deliberate: losing OTA is a
 * worse outcome than needing a power cycle for a fault that has never been
 * observed on this hardware.
 */
namespace LoopWatchdog {

/** Arm with a stall limit in milliseconds. 0 disables. */
void begin(uint32_t limit_ms);

/** Called from the main loop to say it is still running. */
void feed();

/** Called from another context; resets the node if the loop has stalled. */
void check();

/** Stall limit, for reporting. */
uint32_t limitMs();

}
