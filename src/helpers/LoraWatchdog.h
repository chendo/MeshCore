#pragma once

#include <stdint.h>

/**
 * @brief  Finds a radio that has stopped work, and escalates.
 *
 * Repeaters have been found with a transceiver that sent nothing and received
 * nothing for more than an hour. The configuration was correct, and a reboot
 * did not repair it. Nobody found the fault, because from the outside a quiet
 * band and a dead radio look the same. You can tell them apart only if the node
 * makes its own traffic. The node sends a zero-hop advert, and then we examine
 * the airtime of that advert. The watchdog escalates from there. The stages are
 * probe, reinit and reboot.
 *
 * The watchdog is NODE-SCOPED, not per-identity. Under a shared radio the
 * detection part still works: the RX fans out, so every identity sees the full
 * receive airtime, and a dead radio is silent for all of them. The RESPONSE
 * does not work per identity. N identities each send a probe, which is N
 * adverts for one fault. Each identity also decides on its own to reboot the
 * board. Therefore the watchdog injects the three actions that change the
 * world. The composition supplies one of each:
 *
 *   airtime  total TX+RX time on air as the REAL radio saw it. It only
 *            increases. We compare it for change only, never for magnitude.
 *   probe    send one zero-hop advert (slot 0's identity on a multi-head node)
 *   reinit   assemble the transceiver again. Restore every parameter that
 *            begin() applies, not only the driver defaults.
 *   reboot   node-level, one time. Write any deferred data to storage first.
 *
 * Every elapsed-time comparison here is signed, and the watchdog paces every
 * stage. An unpaced retry has already cost a node. An unsigned elapsed test has
 * already cost a node.
 */
class LoraWatchdog {
public:
  typedef uint32_t (*AirtimeFn)(void* ctx);
  typedef void (*ActionFn)(void* ctx);

  /** Arm the watchdog. idle_ms is the silent time that starts the escalation.
   *  A value of 0 leaves the watchdog disarmed. A null airtime source also
   *  leaves it disarmed. */
  void begin(uint32_t idle_ms, void* ctx, AirtimeFn airtime,
             ActionFn probe, ActionFn reinit, ActionFn reboot);

  /** Call this from the main loop. It paces itself. See CHECK_EVERY_MS. */
  void loop();

  bool isArmed() const { return _idle_ms != 0 && _airtime != nullptr; }
  uint32_t reinits() const { return _reinits; }

  enum State : uint8_t { WD_IDLE = 0, WD_TESTING, WD_REINITED };
  State state() const { return (State)_state; }

  static const uint32_t SELFTEST_GRACE_MS = 30000;  // time for a probe to reach the air
  static const uint32_t CHECK_EVERY_MS    = 30000;  // pace. An unpaced retry has cost a node.

private:
  void*     _ctx = nullptr;
  AirtimeFn _airtime = nullptr;
  ActionFn  _probe = nullptr;
  ActionFn  _reinit = nullptr;
  ActionFn  _reboot = nullptr;
  uint32_t  _idle_ms = 0;
  unsigned long _activity_ms = 0;   // the time when the airtime last changed
  unsigned long _last_air = 0;      // the airtime at that time
  unsigned long _next_check_ms = 0;
  unsigned long _test_started_ms = 0;
  unsigned long _test_air = 0;
  uint32_t  _reinits = 0;
  uint8_t   _state = WD_IDLE;
};
