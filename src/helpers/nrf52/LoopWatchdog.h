#pragma once

#include <stdint.h>

/**
 * @brief  Resets the node if the main loop stops.
 *
 * This is not the hardware WDT. You cannot stop the nRF52 watchdog after it
 * starts. It also continues through a soft reset. Therefore it keeps count
 * after the application starts the bootloader. A DFU over BLE needs
 * approximately 200 seconds. Nothing in the Arduino core feeds the hardware
 * watchdog. The bootloader is supplied as a prebuilt binary, so we cannot
 * verify from here whether the bootloader feeds one. If we enabled the hardware
 * watchdog, it could reset a node during an update. The only remote route into
 * this hardware is that same update.
 *
 * This watchdog runs fully inside the application. The main loop calls feed().
 * A dedicated FreeRTOS task at TASK_PRIO_NORMAL wakes one time each second. It
 * resets the node if that stamp is too old. NORMAL is above the loop (LOW).
 * Therefore the task preempts a loop that spins, and the loop cannot starve it.
 *
 * An earlier design made the check only from BLE event context, so each
 * subsystem watched the other. That design failed the one time it was
 * necessary. A node hung with BOTH parts stopped, and the reset button was the
 * only way in. Whatever stops one part can also stop the other. BLE context
 * still calls check(). This does no damage and has no cost, but the task is
 * what makes the guarantee.
 *
 * This watchdog still cannot catch a spin at TASK_PRIO_HIGH (the tasks of
 * Bluefruit). Such a spin starves the loop AND this task. That is the gap that
 * remains. The hardware WDT is the only true answer to it. We do not use the
 * hardware WDT, for the DFU reason above.
 *
 * ARMING: call begin() as early in setup() as you can, with a large boot limit.
 * Everything before that call is unwatched. radio_init(), the filesystem and
 * the I2C probes all have waits with no limit. After the main loop truly runs,
 * setLimit() decreases the limit to the runtime value. A small limit from the
 * start would reset the node during setup on a boot that is slow but correct.
 * Examples are a LittleFS format, or the SoftDevice role ladder.
 */
namespace LoopWatchdog {

/** Arm the watchdog with a stall limit in milliseconds. A value of 0 disables
 *  it. It is safe to call this two times. The code creates the task one time.
 *  A later call only changes the limit. */
void begin(uint32_t limit_ms);

/** Change the stall limit on a watchdog that is already armed. A value of 0
 *  disables it. */
void setLimit(uint32_t limit_ms);

/** The main loop calls this to show that it still runs. */
void feed();

/** Another context calls this. It resets the node if the loop has stalled. */
void check();

/** The stall limit, for reports. */
uint32_t limitMs();

/** True only if the task exists and the limit is not 0. */
bool isArmed();

}
