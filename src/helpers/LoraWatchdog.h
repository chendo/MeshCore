#pragma once

#include <stdint.h>

/**
 * @brief  Notices a radio that has stopped working, and escalates.
 *
 * Repeaters have been found with a transceiver that sent and received nothing
 * for over an hour on correct config, and a reboot did not clear it. Nobody
 * noticed, because from the outside a quiet band and a dead radio look the
 * same. They are only distinguishable if the node makes its own traffic: a
 * zero-hop advert whose airtime we can then check. Staged from there --
 * probe, reinit, reboot.
 *
 * NODE-SCOPED, not per-identity. Under a shared radio the detection half
 * survives sharing (RX fans out, so every identity sees the full receive
 * airtime, and a dead radio goes silent for all of them) but the RESPONSE does
 * not: N identities would each probe -- N adverts for one fault -- and each
 * independently decide to reboot the board. So the three things the watchdog
 * does to the world are injected, and the composition supplies one of each:
 *
 *   airtime  total TX+RX time on air as the REAL radio saw it, monotonic. Only
 *            ever compared for change, never for magnitude.
 *   probe    send one zero-hop advert (slot 0's identity on a multi-head node)
 *   reinit   put the transceiver back together -- and restore every parameter
 *            begin() applies, not just the driver defaults
 *   reboot   node-level, once; flush any deferred writes first
 *
 * Every elapsed-time comparison here is signed and every stage is paced: an
 * unpaced retry and an unsigned elapsed test have each already cost a node.
 */
class LoraWatchdog {
public:
  typedef uint32_t (*AirtimeFn)(void* ctx);
  typedef void (*ActionFn)(void* ctx);

  /** Arm. idle_ms is the silence that starts the escalation; 0 leaves it
   *  disarmed, as does a null airtime source. */
  void begin(uint32_t idle_ms, void* ctx, AirtimeFn airtime,
             ActionFn probe, ActionFn reinit, ActionFn reboot);

  /** Call from the main loop; paces itself, see CHECK_EVERY_MS. */
  void loop();

  bool isArmed() const { return _idle_ms != 0 && _airtime != nullptr; }
  uint32_t reinits() const { return _reinits; }

  enum State : uint8_t { WD_IDLE = 0, WD_TESTING, WD_REINITED };
  State state() const { return (State)_state; }

  static const uint32_t SELFTEST_GRACE_MS = 30000;  // time for a probe to reach the air
  static const uint32_t CHECK_EVERY_MS    = 30000;  // pacing: an unpaced retry has cost a node

private:
  void*     _ctx = nullptr;
  AirtimeFn _airtime = nullptr;
  ActionFn  _probe = nullptr;
  ActionFn  _reinit = nullptr;
  ActionFn  _reboot = nullptr;
  uint32_t  _idle_ms = 0;
  unsigned long _activity_ms = 0;   // when air time last moved
  unsigned long _last_air = 0;      // air time at that moment
  unsigned long _next_check_ms = 0;
  unsigned long _test_started_ms = 0;
  unsigned long _test_air = 0;
  uint32_t  _reinits = 0;
  uint8_t   _state = WD_IDLE;
};
