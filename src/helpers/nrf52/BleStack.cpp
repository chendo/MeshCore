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
      if (Bluefruit.begin(prph, central)) {
        s_prph = prph;
        s_central = central;
      } else {
        /* sd_softdevice_enable() succeeded before the config failed, so the
           stack is up but unusable and a plain retry hits INVALID_STATE. */
        sd_softdevice_disable();
        if (!Bluefruit.begin(1, 0)) return false;
        s_prph = 1;
        s_central = 0;
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
