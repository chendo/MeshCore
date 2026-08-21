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

static bool s_task_started = false;

void setLimit(uint32_t limit_ms) {
  /* If the task never started, begin() deliberately zeroed the limit to say so.
     Do not let a later retune quietly claim a watchdog that does not exist. */
  if (!s_task_started) return;

  /* Stamp first, then widen or narrow. Tightening the limit against a stamp
     that is already stale would reset the node on the very next tick, which is
     precisely wrong at the moment the loop has just proved it is alive. */
  s_last_feed_ms = millis();
  s_limit_ms = limit_ms;
}

void begin(uint32_t limit_ms) {
  s_limit_ms = limit_ms;
  s_last_feed_ms = millis();

  /* Idempotent. begin() is called early in setup() with the boot limit, and a
     second call must not spawn a second task -- two tasks would both be
     checking the same stamp and the stack cost would be paid twice. */
  if (s_task_started) return;

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
  /* Return value checked. A silent failure here leaves the node with no
     watchdog for that entire boot and nothing anywhere saying so -- the one
     failure mode a watchdog must not have. */
  if (xTaskCreate(watchdog_task, "wdog", 256, NULL, TASK_PRIO_NORMAL, NULL) == pdPASS) {
    s_task_started = true;
  } else {
    s_limit_ms = 0;                  // honest: not armed
    Serial.println("WDOG: task create FAILED -- node is unwatched");
  }
}

bool isArmed() { return s_task_started && s_limit_ms != 0; }

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
