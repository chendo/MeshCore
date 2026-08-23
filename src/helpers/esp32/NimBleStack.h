#pragma once

#include <stdint.h>

/**
 * @brief  One place that starts NimBLE with the roles a bridge needs.
 *
 * The ESP32 counterpart to BleStack, and deliberately much smaller. The nRF52
 * version exists because the SoftDevice takes its RAM from a region the linker
 * fixed at build time, so a request that is too large fails at run time and
 * leaves the node with no CLI and no DFU. That whole ladder has no counterpart
 * here: NimBLE's connection count is a compile-time setting
 * (CONFIG_BT_NIMBLE_MAX_CONNECTIONS), its buffers come from the heap, and a
 * build that asks for more than it configured does not link a working image
 * rather than failing on a mast.
 *
 * What survives is the reason the file exists at all: ONE owner decides how
 * many connections the node can hold, and it decides once.
 */
namespace NimBleStack {

/**
 * @param name     the GAP device name.
 * @param prph     peripheral links: 1 for a peer that dials IN, plus any that
 *                 something else on the build needs.
 * @param central  outward links.
 * @returns false when NimBLE would not start. There is then no BLE.
 */
bool ensure(const char* name, uint8_t prph, uint8_t central);

/** The slots this node will use. Capped at what NimBLE was configured for. */
uint8_t periphSlots();
uint8_t centralSlots();

/** The ATT MTU asked for. BleLink turns it into a write size, and at 247 a
 *  whole bridge frame is one write and never becomes fragments. */
uint16_t mtu();

/** The GAP name this node advertises. Kept here because the advert has to
 *  carry it and has to truncate it to fit. Never null. */
const char* name();

}
