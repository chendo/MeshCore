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
 * This runs entirely inside the application. feed() is called from the main
 * loop; a dedicated FreeRTOS task at TASK_PRIO_NORMAL wakes once a second and
 * resets the node if that stamp has gone stale. NORMAL sits above the loop
 * (LOW), so it preempts a spinning loop and cannot be starved by it.
 *
 * An earlier design checked only from BLE event context, so each subsystem
 * watched the other. That failed the one time it mattered -- a node hung with
 * BOTH stopped and no way in short of the reset button -- because whatever
 * wedges one can wedge the other. check() is still called from BLE context as
 * well, which is harmless and costs nothing, but the task is what makes the
 * guarantee.
 *
 * What it still cannot catch is a spin at TASK_PRIO_HIGH (Bluefruit's own
 * tasks), which starves the loop AND this task. That is the remaining gap and
 * the hardware WDT is the only real answer to it -- not taken, for the DFU
 * reason above.
 *
 * ARMING: begin() as early in setup() as possible, with a generous boot limit,
 * because everything before it is unwatched -- radio_init(), the filesystem,
 * and the I2C probes all have unbounded waits. Once the main loop is genuinely
 * running, setLimit() tightens to the runtime limit. Arming tight from the
 * start would reset the node mid-setup on a slow-but-legitimate boot (a
 * LittleFS format, or the SoftDevice role ladder).
 */
namespace LoopWatchdog {

/** Arm with a stall limit in milliseconds. 0 disables. Safe to call twice --
 *  the task is created once; a later call only retunes the limit. */
void begin(uint32_t limit_ms);

/** Retune the stall limit on an already-armed watchdog. 0 disables. */
void setLimit(uint32_t limit_ms);

/** Called from the main loop to say it is still running. */
void feed();

/** Called from another context; resets the node if the loop has stalled. */
void check();

/** Stall limit, for reporting. */
uint32_t limitMs();

/** True only if the task actually exists and a non-zero limit is set. */
bool isArmed();

}
