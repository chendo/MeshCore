#include "LoopWatchdog.h"

#include <Arduino.h>

namespace LoopWatchdog {

static volatile uint32_t s_last_feed_ms = 0;
static uint32_t s_limit_ms = 0;

static void watchdog_task(void* arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    check();
  }
}

void begin(uint32_t limit_ms) {
  s_limit_ms = limit_ms;
  s_last_feed_ms = millis();

  /* Its own task, not a callback from somewhere convenient.
     
     The first version checked only from BLE event context, on the assumption
     that BLE would keep running while the loop stalled -- each subsystem
     watching the other. Then a node hung with BOTH stopped: USB still
     enumerating, no CLI, no advertising, nothing left to notice, and no way in
     without pressing its reset button. Mutual liveness fails exactly when it is
     needed, because whatever wedges one can wedge the other.
     
     TASK_PRIO_NORMAL sits above the loop (LOW) and below Bluefruit (HIGH), so
     it preempts a spinning loop and cannot be starved by it, while never
     delaying radio work. It wakes once a second and compares two numbers. */
  xTaskCreate(watchdog_task, "wdog", 256, NULL, TASK_PRIO_NORMAL, NULL);
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
