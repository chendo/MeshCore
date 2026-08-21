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
// The radio settings (freq/bw/sf/cr, TX power, CAD) are properties of the NODE,
// not of an identity: there is one transceiver and it can only be on one
// channel. Slot 0's prefs own them; SharedRadioCore reconciles the CAD and
// interference-threshold votes (see applyRadioPolicy).
//
// COST OF A SLOT, measured on RAK_3401_hydra (nRF52840, 235,520 B RAM) by
// building at HYDRA_NUM_SLOTS=2 and again at 3:
//   static (.bss)  +5,568 B per chat slot — the ChatSlot object; reserved
//                  whether or not the slot is enabled
//   heap           +2,296 B per ENABLED chat slot — 8 x sizeof(mesh::Packet)
//                  (262 B) plus the three pool queues; zero while disabled
//   flash          +192 B per slot
// ~7.9 KB all-in against slot 0's ~20 KB (mesh 9,392 + tables 1,296 + 32-entry
// pool ~9,250). Most of the 5,568 is BaseChatMesh's contact table, so
// MAX_CONTACTS is the dial that matters if it needs to come down further.

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

class HydraNode {
public:
  HydraNode();

  void begin(FILESYSTEM* fs);
  void loop();

  // Node CLI: `slots`, `stats-shared`, `stats-txwait`, `trace`, `slot N on|off`,
  // `slot N <cmd>`. Anything else falls through to slot 0, so the familiar
  // repeater CLI still works unqualified on a hydra node.
  void handleCommand(char* command, char* reply, size_t reply_sz);

  bool hasPendingWork() const;

  RepeaterSlot& repeater() { return _slot0; }
  SharedRadioCore& radio() { return _core; }

private:
  bool startSlot(int idx);
  void stopSlot(int idx);
  void loadSlotConfig();
  void saveSlotConfig();
  void formatSlotTable(char* reply, size_t reply_sz);
  static const char* typeName(SlotType t);
  static void slotIdName(int idx, char* dest, size_t sz);

  SharedRadioCore _core;
  RepeaterSlot    _slot0;
#if HYDRA_NUM_CHAT_SLOTS > 0
  ChatSlot        _chat[HYDRA_NUM_CHAT_SLOTS];
#endif
#ifdef LORA_WATCHDOG_MS
  LoraWatchdog    _lora_wd;   // node-scoped: one per board, not one per slot
#endif
  HydraSlot*      _slots[HYDRA_NUM_SLOTS];
  SlotType        _cfg[HYDRA_NUM_SLOTS];   // configured type; _slots[i]->type() is the live one
  FILESYSTEM*     _fs;
};

extern HydraNode hydra;
