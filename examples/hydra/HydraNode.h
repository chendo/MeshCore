#pragma once

// Hydra: one board, one LoRa radio, several mesh identities.
//
//   Node
//   ├── board, radio driver, RTC, RNG, filesystem      (one each, node-scoped)
//   ├── SharedRadioCore ── one RadioPort per slot
//   ├── node-scoped modules: LoopWatchdog, LoRa watchdog, StatusLed,
//   │                        I2CBusRecovery, MeshObserver
//   └── slots[HYDRA_NUM_SLOTS]
//         slot 0    : repeater — always on
//         slot 1..N : chat | off
//
// The radio settings are properties of the NODE. They are freq, bw, sf, cr, the
// TX power and CAD. They are not properties of an identity. There is one
// transceiver, and it can be on only one channel. The prefs of slot 0 own these
// settings. SharedRadioCore resolves the votes for CAD and for the interference
// threshold. See applyRadioPolicy.
//
// THE COST OF A SLOT. These figures come from RAK_3401_hydra (nRF52840,
// 235,520 B RAM). We built the firmware at HYDRA_NUM_SLOTS=2 and again at 3:
//   static (.bss)  +5,640 B for each chat slot. This is the ChatSlot object.
//                  The build reserves it whether the slot is enabled or not.
//   heap           +2,352 B for each ENABLED chat slot. This is
//                  8 x sizeof(mesh::Packet) (262 B) plus 256 B for the three
//                  pool queues. It is zero while the slot is disabled.
//   flash          no measurable change for each slot
// The total is about 7.9 KB, against about 20 KB for slot 0 (mesh 9,392 +
// tables 1,296 + the 32-entry pool at about 9,250). Most of the 5,640 B is the
// contact table of BaseChatMesh. So MAX_CONTACTS is the setting that matters if
// the cost must come down more.

#include "ChatSlot.h"
#include "HydraSlot.h"
#include "RepeaterSlot.h"
#ifdef LORA_WATCHDOG_MS
  #include <helpers/LoraWatchdog.h>
#endif

#ifndef HYDRA_NUM_SLOTS
  #define HYDRA_NUM_SLOTS  2
#endif
#if HYDRA_NUM_SLOTS < 1 || HYDRA_NUM_SLOTS > 8
  #error "HYDRA_NUM_SLOTS must be 1..8 (SharedRadioCore::MAX_PORTS)"
#endif
#define HYDRA_NUM_CHAT_SLOTS  (HYDRA_NUM_SLOTS - 1)

#define HYDRA_SLOT_CFG_FILE  "/hydra_slots"

// This holds everything about a slot that stays after a reboot. The identity
// itself is not here. IdentityStore files the identity under a name that comes
// from the slot INDEX only. Thus nothing in this record can move a keypair
// (decision 7).
struct SlotConfig {
  SlotType type;
  uint8_t  advert_mins;   // 0 = never advert again
  bool     flood;         // flood the regular advert, and do not send it zero-hop
  bool     diag;          // the diagnostic bot answers ping/trace/peers. Off by default.
  char     name[SLOT_NAME_MAX];
};

class HydraNode {
public:
  HydraNode();

  void begin(FILESYSTEM* fs);
  void loop();

  // The node CLI has `slots`, `peers`, `stats-shared`, `stats-txwait`, `trace`
  // and `slot N ...`. Every other command goes to slot 0. Thus the familiar
  // repeater CLI still works on a hydra node without a slot prefix. That
  // fall-through IS the node namespace. The prefs of slot 0 own the one radio
  // (decision 8).
  //
  // sender_timestamp follows the convention of upstream. A value of 0 means the
  // serial console. A value that is not 0 means that the command came over the
  // air. A command that reports on third parties works only on the serial
  // console (decision E).
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply, size_t reply_sz);

  bool hasPendingWork() const;
  void flushPendingWrites();

  RepeaterSlot& repeater() { return _slot0; }
  SharedRadioCore& radio() { return _core; }

private:
  SlotEnableResult startSlot(int idx);
  void stopSlot(int idx);
  void loadSlotConfig();
  void saveSlotConfig();
  void formatSlotTable(char* reply, size_t reply_sz);
  void handleSlotCommand(int idx, uint32_t sender_timestamp, char* arg,
                         char* reply, size_t reply_sz);
  void handleSlotSet(int idx, uint32_t sender_timestamp, char* arg, char* reply, size_t reply_sz);
  void handleSlotGet(int idx, uint32_t sender_timestamp, char* arg, char* reply, size_t reply_sz);
  bool setSlotPrivateKey(int idx, const char* hex, char* reply, size_t reply_sz);
  void reportPeers(char* reply, size_t reply_sz);
#ifdef CLOCK_CONVERGE_MS
  void loopClockConverge();
  void reportClocks(char* reply, size_t reply_sz);
#endif
  static const char* typeName(SlotType t);
  static void slotIdName(int idx, char* dest, size_t sz);

  SharedRadioCore _core;
  RepeaterSlot    _slot0;
#if HYDRA_NUM_CHAT_SLOTS > 0
  ChatSlot        _chat[HYDRA_NUM_CHAT_SLOTS];
#endif
#ifdef LORA_WATCHDOG_MS
  LoraWatchdog    _lora_wd;   // node-scoped: one for each board, not one for each slot
#endif
#ifdef CLOCK_CONVERGE_MS
  /* CLOCK CONVERGENCE. The node steers its own clock towards what its
     neighbours say the time is. The rules live in helpers/ClockPolicy.h as free
     functions, and a host test covers every one of them. What is here is only
     the plumbing: it reads the samples out of the observer, carries the counters
     that the rules need, and writes the RTC.

     It belongs to the NODE and not to a slot. There is one clock on this board
     and several identities on it, so a slot that set the clock would set it for
     every other slot as well. The observer is node-scoped for the same reason,
     and it already holds a reading from every advert that this radio heard.

     Defining CLOCK_CONVERGE_MS compiles the feature in and sets how often it
     runs. It is still OFF until somebody types "clocks on", so a build that
     carries it does not act on it by accident. */
  /* How often the node looks while its clock is unset. A clock that nobody has
     ever set makes the node invisible and not merely wrong: its adverts carry
     timestamps from 2024 that every peer rejects as a replay. So it looks more
     often in that state, because every round it stays there is a round off the
     air. */
  static const uint32_t CLOCK_CONVERGE_FAST_MS = 30UL * 1000UL;

  /* A gap this large between what the clock reads and what it should read did
     not come from a crystal. Something SET the clock: a person with "clock
     sync" or "time", a client app, GPS or NTP. Convergence then stands down for
     mesh::CLOCK_ADMIN_HOLD_MS.

     Watching the clock catches every one of those paths. A callback on the CLI
     would catch only the CLI, and it would tie examples/simple_repeater to
     hydra, which nothing else does. 5s is far above the quantisation of a clock
     that reads to the second and above the jitter of a hardware RTC against
     millis(), and far below any set that a person would make. */
  static const int32_t  CLOCK_EXTERNAL_SET_S = 5;

  bool     _clock_converge = false;    // off until "clocks on"
  uint32_t _clock_prev_ms = 0;         // millis() at the previous pass
  uint32_t _clock_due_ms = 0;          // time owed before the next estimate
  uint32_t _clock_since_move_ms = 0;   // feeds the slew allowance
  uint32_t _clock_admin_hold_ms = 0;   // what is left of the 7-day admin hold
  uint32_t _clock_expect_s = 0;        // what the clock read on the previous pass
  bool     _clock_expect_ok = false;
  uint32_t _clock_steps = 0, _clock_slews = 0, _clock_admin_sets = 0;
  int32_t  _clock_last_adj_s = 0;
  uint8_t  _clock_last_hold = 0;
#endif
  HydraSlot*      _slots[HYDRA_NUM_SLOTS];
  SlotConfig      _cfg[HYDRA_NUM_SLOTS];   // the config. _slots[i]->type() is the live type.
  FILESYSTEM*     _fs;
};

extern HydraNode hydra;
