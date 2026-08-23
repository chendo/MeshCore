#include "NimBleStack.h"

#include <NimBLEDevice.h>
#include <string.h>

namespace NimBleStack {

/* MTU 247 makes a bridge frame of up to 244 bytes one write, so it never
   becomes fragments and the whole class of reassembly faults cannot occur. The
   nRF52 side reaches the same number through a ladder that trades MTU against
   RAM; here it is simply asked for. */
static const uint16_t WANT_MTU = 247;

static bool s_done = false;
static uint8_t s_prph = 1, s_central = 0;
/* Long enough for "MeshCore-" plus the longest node name the CLI accepts. */
static char s_name[48] = { 0 };

bool ensure(const char* name, uint8_t prph, uint8_t central) {
  if (!s_done) {
    if (prph < 1) prph = 1;

    /* Every connection, inbound and outward, comes out of one pool. A build
       that asks for more than it configured would dial a peer it can never
       hold, so cap the promise here rather than fail later on the air. */
    const uint8_t total = (uint8_t)(prph + central);
    if (total > CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
      const uint8_t spare = (uint8_t)(CONFIG_BT_NIMBLE_MAX_CONNECTIONS - prph);
      central = (prph < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) ? spare : 0;
    }

    if (!NimBLEDevice::isInitialized()) {
      if (!NimBLEDevice::init(name != nullptr ? name : "")) return false;
      NimBLEDevice::setMTU(WANT_MTU);
    }
    s_prph = prph;
    s_central = central;
    s_done = true;
  }
  if (name != nullptr) {
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = 0;
    NimBLEDevice::setDeviceName(name);
  }
  return true;
}

uint8_t periphSlots()  { return s_prph; }
uint8_t centralSlots() { return s_central; }
uint16_t mtu()         { return NimBLEDevice::getMTU(); }
const char* name()     { return s_name; }

}
