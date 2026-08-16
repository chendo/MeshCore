#include "LoopWatchdog.h"

#include <Arduino.h>

namespace LoopWatchdog {

static volatile uint32_t s_last_feed_ms = 0;
static uint32_t s_limit_ms = 0;

void begin(uint32_t limit_ms) {
  s_limit_ms = limit_ms;
  s_last_feed_ms = millis();
}

void feed() {
  s_last_feed_ms = millis();
}

uint32_t limitMs() { return s_limit_ms; }

void check() {
  if (s_limit_ms == 0) return;
  uint32_t last = s_last_feed_ms;
  if (last == 0) return;                       // not armed yet
  if ((uint32_t)(millis() - last) < s_limit_ms) return;

  /* The loop has not advanced for the stall limit while this context still
     runs. Nothing local can recover that, and a repeater stuck here is off the
     air until somebody visits it.

     Same call NRF52Board::reboot() uses, so a watchdog reset is indistinguishable
     from the reboot command rather than being a second, subtly different path. */
  NVIC_SystemReset();
}

}
