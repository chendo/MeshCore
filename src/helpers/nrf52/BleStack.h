#pragma once

#include <stdint.h>

/**
 * @brief  One place that brings the SoftDevice up, with the roles we asked for.
 *
 * Two subsystems want the BLE stack -- the diagnostic/DFU serial interface and
 * the bridge -- and whichever runs first decides how many connections the node
 * can ever hold. Role counts cannot be changed afterwards without taking the
 * SoftDevice down, so the decision has to be made once, here, rather than by
 * whoever happens to initialise first.
 *
 * Getting it wrong is not a soft failure. Role counts drive the SoftDevice's
 * RAM requirement, which is fixed at link time (RAM origin 0x20006000 leaves it
 * 24KB), and asking for more than fits makes Bluefruit.begin() return false
 * with the stack half-configured -- no CLI, no DFU, and on a repeater with no
 * USB attached, no way back in at all. So a failure retries with the
 * single-peripheral configuration that has always worked: the node degrades to
 * broadcast-only bridging instead of going silent.
 */
namespace BleStack {

/**
 * @param name     GAP device name, or NULL to leave it alone.
 * @param prph     peripheral links wanted: 1 for the CLI, plus one for each
 *                 peer expected to connect INWARD to us.
 * @param central  outward links we want to be able to open.
 * @returns false only if even the fallback failed, meaning there is no BLE.
 */
bool ensure(const char* name, uint8_t prph, uint8_t central);

/** What the stack actually came up with, which may be less than requested. */
uint8_t periphSlots();
uint8_t centralSlots();

}
