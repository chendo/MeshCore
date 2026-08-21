#include "LoraWatchdog.h"

#include <Arduino.h>
#include <MeshCore.h>

void LoraWatchdog::begin(uint32_t idle_ms, void* ctx, AirtimeFn airtime,
                         ActionFn probe, ActionFn reinit, ActionFn reboot) {
  _idle_ms = idle_ms;
  _ctx = ctx;
  _airtime = airtime;
  _probe = probe;
  _reinit = reinit;
  _reboot = reboot;
}

void LoraWatchdog::loop() {
  if (!isArmed()) return;

  unsigned long now = millis();
  if (_next_check_ms != 0 && (long)(now - _next_check_ms) < 0) return;
  _next_check_ms = now + CHECK_EVERY_MS;

  unsigned long air = _airtime(_ctx);

  if (air != _last_air) {                 // this is proof that the radio works
    _last_air = air;
    _activity_ms = now;
    _state = WD_IDLE;
    return;
  }
  if (_activity_ms == 0) { _activity_ms = now; return; }

  switch (_state) {
    case WD_IDLE:
      if ((long)(now - _activity_ms) < (long)_idle_ms) return;
      /* Make our own traffic. Do not wait for the traffic of another node. On
         a quiet band no other node transmits. We would then read the silence
         incorrectly as a dead radio. */
      _test_air = air;
      _test_started_ms = now;
      _state = WD_TESTING;
      if (_probe) _probe(_ctx);
      MESH_DEBUG_PRINTLN("LoRa watchdog: silent %lus, probing radio",
                         (unsigned long)((now - _activity_ms) / 1000));
      return;

    case WD_TESTING:
      if ((long)(now - _test_started_ms) < (long)SELFTEST_GRACE_MS) return;
      if (air != _test_air) {             // the radio transmitted, so it is alive
        _last_air = air;
        _activity_ms = now;
        _state = WD_IDLE;
        return;
      }
      /* We asked the radio to transmit, and no airtime resulted. Do a reinit.
         The injected hook must restore every parameter that begin() sets. A
         bare radio_init() leaves the node on the default frequency of the
         driver. The node is then off-band, and it gives no indication of this.
         That result is worse than the fault it repairs. */
      _reinits++;
      MESH_DEBUG_PRINTLN("LoRa watchdog: no airtime after probe, reinitialising");
      if (_reinit) _reinit(_ctx);
      _test_started_ms = now;
      _test_air = _airtime(_ctx);         // read again: a reinit can reset the counters
      _state = WD_REINITED;
      if (_probe) _probe(_ctx);
      return;

    case WD_REINITED:
      if ((long)(now - _test_started_ms) < (long)SELFTEST_GRACE_MS) return;
      if (air != _test_air) {             // the reinit worked
        _last_air = air;
        _activity_ms = now;
        _state = WD_IDLE;
        return;
      }
      /* The reinit is complete, and the radio still cannot transmit. Nothing
         else here can help. A repeater that cannot use its radio does no work
         at all. */
      MESH_DEBUG_PRINTLN("LoRa watchdog: dead after reinit, rebooting");
      if (_reboot) _reboot(_ctx);
      return;
  }
}
