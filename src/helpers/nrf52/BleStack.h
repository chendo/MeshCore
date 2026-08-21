#pragma once

#include <stdint.h>

/**
 * @brief  One place that starts the SoftDevice with the roles we ask for.
 *
 * Two subsystems want the BLE stack: the diagnostic and DFU serial interface,
 * and the bridge. The one that runs first fixes how many connections the node
 * can ever hold. Role counts cannot change later unless the SoftDevice stops
 * first. Make the decision once, here, and not in whichever subsystem starts
 * first.
 *
 * A wrong decision is not a soft failure. Role counts set the RAM that the
 * SoftDevice needs. The linker fixes that RAM at build time: RAM origin
 * 0x20006000 leaves 24KB. A request for more RAM than the linker reserved makes
 * Bluefruit.begin() return false and leaves the stack half configured. The node
 * then has no CLI and no DFU. A repeater with no USB cable has no way back in.
 * A failure thus causes a retry with the single-peripheral configuration, which
 * always works. The node keeps its CLI and loses only its bridge links.
 */
namespace BleStack {

/**
 * @param name     the GAP device name, or NULL to keep the present name.
 * @param prph     peripheral links to request: 1 for the CLI, and 1 more for
 *                 each peer that connects INWARD to us.
 * @param central  outward links to request.
 * @returns false only if the fallback also failed. There is then no BLE.
 */
bool ensure(const char* name, uint8_t prph, uint8_t central);

/** The slots that the stack gave us. A count can be less than the request. */
uint8_t periphSlots();
uint8_t centralSlots();

/**
 * The ATT MTU and the TX queue depth that the stack agreed to.
 *
 * The ladder in ensure() gives up both of these BEFORE it gives up connection
 * slots. These functions thus report what the node accepted. Read them: an MTU
 * of 23 makes the link cut every frame into fragments, and a queue depth of 1
 * stops the link from sending those fragments back to back. Together they
 * discarded one third of all link traffic.
 */
uint16_t mtu();
uint8_t txQueueSize();

}
