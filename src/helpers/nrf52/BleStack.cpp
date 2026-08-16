#include "BleStack.h"

#include <bluefruit.h>

namespace BleStack {

static bool s_done = false;
static uint8_t s_prph = 1, s_central = 0;

bool ensure(const char* name, uint8_t prph, uint8_t central) {
  if (!s_done) {
    /* Only bring it up if nobody already has -- a second Bluefruit.begin()
       would fail on INVALID_STATE and take the caller down with it. */
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (!sd_enabled) {
      if (prph < 1) prph = 1;                    // the CLI always needs one

      /* Walk down until the SoftDevice's RAM requirement fits what the linker
         reserved, rather than guessing at build time. Measured on a RAK3401:
         (4,3) does not fit the 24KB at RAM origin 0x20006000 and the node came
         up with a single peripheral, so the ceiling is genuinely lower than it
         looks and worth discovering rather than assuming.

         Each failed attempt leaves the SoftDevice enabled but unconfigured --
         sd_softdevice_enable() succeeds before the config does not -- so it has
         to come down before the next try or that one fails on INVALID_STATE. */
      bool up = false;
      while (!up) {
        if (Bluefruit.begin(prph, central)) {
          s_prph = prph;
          s_central = central;
          up = true;
          break;
        }
        sd_softdevice_disable();
        /* Shed INBOUND links first and keep the outbound ones. Measured on a
           RAK3401: (4,3) down to (4,0) were all refused and (3,0) accepted, so
           roughly three connections fit -- but spending all three on peers
           dialling in leaves none to dial out with, which is the half that
           actually needs a slot reserved. A node can always be reached on its
           one remaining peripheral slot (the CLI), and a peer that cannot
           connect inward to us will be connected to BY us instead. */
        if (prph > 1) {
          prph--;
        } else if (central > 0) {
          central--;
        } else {
          return false;                          // even (1,0) refused: no BLE at all
        }
      }
    }
    s_done = true;
  }
  if (name != nullptr) Bluefruit.setName(name);
  return true;
}

uint8_t periphSlots()  { return s_prph; }
uint8_t centralSlots() { return s_central; }

}
