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
  /* If the task never started, begin() set the limit to 0 to show this. Do not
     let a later change claim a watchdog that does not exist. */
  if (!s_task_started) return;

  /* Write the stamp first, then increase or decrease the limit. A smaller
     limit against a stamp that is already too old would reset the node on the
     next tick. That result is wrong, because the loop has just given proof
     that it is alive. */
  s_last_feed_ms = millis();
  s_limit_ms = limit_ms;
}

void begin(uint32_t limit_ms) {
  s_limit_ms = limit_ms;
  s_last_feed_ms = millis();

  /* This function is idempotent. setup() calls begin() early with the boot
     limit. A second call must not create a second task. Two tasks would check
     the same stamp, and the node would pay the stack cost two times. */
  if (s_task_started) return;

  /* The watchdog uses its own task. It is not a callback from a convenient
     place.

     The first version made the check only from BLE event context. It assumed
     that BLE would continue to run while the loop stalled, and that each
     subsystem would watch the other. Then a node hung with BOTH parts stopped.
     USB still enumerated, but there was no CLI, no advertising and nothing
     left to find the fault. The reset button was the only way in. Mutual
     liveness fails at the exact time you need it, because whatever stops one
     part can also stop the other.

     TASK_PRIO_NORMAL is above the loop (LOW) and below Bluefruit (HIGH).
     Therefore the task preempts a loop that spins, and the loop cannot starve
     it. The task also never delays radio work. It wakes one time each second
     and compares two numbers. */
  /* The code checks the return value. A failure with no report leaves the node
     with no watchdog for that full boot, and nothing says so. That is the one
     failure mode a watchdog must not have. */
  if (xTaskCreate(watchdog_task, "wdog", 256, NULL, TASK_PRIO_NORMAL, NULL) == pdPASS) {
    s_task_started = true;
  } else {
    s_limit_ms = 0;                  // this reports the truth: not armed
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
  if (last == 0) return;                       // the watchdog is not armed yet
  if ((uint32_t)(millis() - last) < s_limit_ms) return;

  /* The loop has not advanced for the stall limit, and this context still
     runs. Nothing local can recover from that. A repeater that stops here is
     off the air until a person visits it.

     This is the same call that NRF52Board::reboot() uses. Therefore a watchdog
     reset is the same as the reboot command. It is not a second path with
     small differences. */
  NVIC_SystemReset();
}

}
