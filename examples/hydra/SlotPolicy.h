#pragma once

// This file has the rules that a slot must satisfy before you can enable it.
// It also has the split between the node CLI namespace and the slot CLI
// namespace. The file is separate from the firmware, so you can test both on a
// host. No part of this file includes an Arduino, Mesh or target header.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum SlotType : uint8_t {
  SLOT_OFF      = 0,
  SLOT_REPEATER = 1,
  SLOT_CHAT     = 2,
  SLOT_ROOM     = 3,   // a room server: its own identity, ACL and post buffer
};

// This is the same as NodePrefs::node_name. Slot 0 and slots 1..N thus have the
// same limit.
#define SLOT_NAME_MAX  32

// This is the node-level RAM reserve floor of decision G. When you enable a
// slot, it must leave at least this much heap. The heap is for BLE connections,
// for LittleFS and for the constant allocation and release of packets.
#ifndef HYDRA_RAM_RESERVE
  #define HYDRA_RAM_RESERVE  24576
#endif

// ---------------------------------------------------------------- slot naming

// These are the same character rules as isValidName() in CommonCLI. These
// characters break the advert listing formats that the apps use.
inline bool slotNameValid(const char* n) {
  if (n == nullptr || n[0] == 0) return false;          // an empty name is not a name
  if (strlen(n) >= SLOT_NAME_MAX) return false;
  for (; *n; n++) {
    if (*n == '[' || *n == ']' || *n == '\\' || *n == ':' ||
        *n == ',' || *n == '?' || *n == '*') return false;
  }
  return true;
}

// ------------------------------------------------------------- the enable gate

enum SlotEnableResult : uint8_t {
  SLOT_ENABLE_OK = 0,
  SLOT_ENABLE_RANGE,      // there is no such slot
  SLOT_ENABLE_SLOT0,      // slot 0 is the repeater, and you never toggle it
  SLOT_ENABLE_NO_NAME,    // decision H: an identity with no name must not exist
  SLOT_ENABLE_BAD_TYPE,
  SLOT_ENABLE_NO_RAM,     // the slot would go below the reserve floor
  SLOT_ENABLE_FAILED,     // begin() refused, for its own reasons
};

inline const char* slotEnableError(SlotEnableResult r) {
  switch (r) {
    case SLOT_ENABLE_OK:       return "OK";
    case SLOT_ENABLE_RANGE:    return "ERR: no such slot";
    case SLOT_ENABLE_SLOT0:    return "ERR: slot 0 is the repeater and is always on";
    case SLOT_ENABLE_NO_NAME:  return "ERR: slot has no name - set it first: slot N set name <text>";
    case SLOT_ENABLE_BAD_TYPE: return "ERR: type must be chat or room";
    case SLOT_ENABLE_NO_RAM:   return "ERR: not enough free RAM above the node reserve";
    case SLOT_ENABLE_FAILED:   return "ERR: slot failed to start";
  }
  return "ERR";
}

// This function decides everything that does not need the heap or the radio.
// RamFloor below checks the RAM floor separately. The only true answer to the
// question "is there room" is to try to allocate the memory.
inline SlotEnableResult slotEnableCheck(int idx, int num_slots, SlotType t, const char* name) {
  if (idx <= 0 || idx >= num_slots) {
    return (idx == 0) ? SLOT_ENABLE_SLOT0 : SLOT_ENABLE_RANGE;
  }
  if (t != SLOT_CHAT && t != SLOT_ROOM) return SLOT_ENABLE_BAD_TYPE;
  if (!slotNameValid(name)) return SLOT_ENABLE_NO_NAME;
  return SLOT_ENABLE_OK;
}

// ---------------------------------------------- how to decode a stored record
//
// Safe mode (decision D). Every value that the node cannot read, and every
// unexpected value, must give "slot off". It must never give a different slot
// type. It must never start an identity that the config did not describe.

// This build must not give a new meaning to a type byte that it does not
// implement.
inline SlotType slotTypeFromByte(uint8_t b) {
  return (b == SLOT_CHAT || b == SLOT_ROOM) ? (SlotType)b : SLOT_OFF;
}

// A stored record enables a slot only if the record also passes the gate of the
// CLI. The rule that no identity has an empty name thus stays true after a
// reboot and after a corrupt file.
inline SlotType slotTypeFromRecord(uint8_t type_byte, const char* name) {
  SlotType t = slotTypeFromByte(type_byte);
  return slotNameValid(name) ? t : SLOT_OFF;
}

// ------------------------------------------------------------ the RAM reserve
//
// Decision G does not let the code set any size from a measurement of the free
// heap. The free heap at boot is not the free heap at peak load, and the
// failures are silent. This class does not measure the heap. It PINS the
// reserve so that nothing else can take it. The slot then allocates from what
// is left. A slot that starts has thus kept the floor intact. The node refuses
// a slot that cannot keep the floor. It refuses that slot now, and does not
// drop packets at 3am.

class RamFloor {
public:
  typedef void* (*AllocFn)(size_t);
  typedef void  (*FreeFn)(void*);

  RamFloor(size_t bytes, AllocFn a = ::malloc, FreeFn f = ::free)
      : _free_fn(f), _p(nullptr), _wanted(bytes) {
    if (bytes > 0) _p = a(bytes);
  }
  ~RamFloor() { release(); }

  // False means that the code could not pin the floor. The heap is already at
  // the reserve or below it. Do not start a slot.
  bool held() const { return _wanted == 0 || _p != nullptr; }
  void release() { if (_p) { _free_fn(_p); _p = nullptr; } }

private:
  RamFloor(const RamFloor&);
  RamFloor& operator=(const RamFloor&);
  FreeFn _free_fn;
  void*  _p;
  size_t _wanted;
};

// This is the largest single block that the heap still gives out. It fills the
// headroom column of the `slots` command. It is a report only. Nothing sets its
// own size from this value.
inline size_t probeLargestBlock(size_t ceiling,
                                RamFloor::AllocFn a = ::malloc,
                                RamFloor::FreeFn f = ::free) {
  size_t lo = 0, hi = ceiling;
  while (lo < hi) {
    size_t mid = lo + (hi - lo + 1) / 2;
    void* p = a(mid);
    if (p) { f(p); lo = mid; } else { hi = mid - 1; }
  }
  return lo;
}

// ------------------------------------------------------- CLI namespace split
//
// Decision 8. The node level owns everything that one piece of hardware
// supplies. That is the transceiver, the board and the flash. The node refuses
// a slot that asks for one of these. If it did not refuse, the slot would
// retune the antenna under every other slot without a warning.

inline bool slotVerbIsNodeLevel(const char* verb) {
  static const char* const kNodeOnly[] = {
    // the one transceiver
    "freq", "bw", "sf", "cr", "tx", "radio", "cad", "int.thresh", "extra.sf",
    "agc.reset.interval", "dutycycle", "af",
    // the one board
    "adc.multiplier", "reboot", "erase", "start", "powersave", "bridge",
    // the state of the whole node
    "time", "clock", "log", "password", "ver", "neighbors",
    "discover.neighbors",
  };
  if (verb == nullptr || *verb == 0) return false;
  size_t vlen = 0;
  while (verb[vlen] && verb[vlen] != ' ') vlen++;
  for (size_t i = 0; i < sizeof(kNodeOnly) / sizeof(kNodeOnly[0]); i++) {
    size_t klen = strlen(kNodeOnly[i]);
    if (vlen < klen || memcmp(verb, kNodeOnly[i], klen) != 0) continue;
    // an exact token, or a child of one after a dot ("radio.rxgain" under "radio")
    if (vlen == klen || verb[klen] == '.') return true;
  }
  return false;
}

inline const char* slotNodeLevelError() {
  return "ERR: node-level setting - use it unqualified, not under a slot";
}

// -------------------------------------------------------------- advert phase
//
// Every enabled slot floods an advert (decision C), and each slot runs its own
// interval. Nothing else spreads them, so all the identities on one board
// transmit together, and they do it again at every interval boundary. That
// costs more than it looks. The board has one half-duplex antenna, so the
// adverts contend with each other and with the routed traffic of slot 0, which
// has priority. A flood advert also spends airtime on every node that hears
// it. And the duty-cycle pool of SharedRadioCore drains in a lump.
//
// The phase of a slot comes from the public key of its identity, and not from
// the slot index. IdentityStore holds that key, so the phase is the same after
// a reboot. The key is also unique to the identity, so two hydra nodes in
// range spread against each other as well as against themselves. An index
// would give every node the same set of phases, and a site that comes back
// after a power cut would put all of them in step.

// The first 2 bytes of a public key, as a fraction of 65536. A key is random,
// so this value is flat across the range.
inline uint16_t advertPhaseFraction(const uint8_t* pub_key, size_t key_len) {
  if (pub_key == nullptr || key_len < 2) return 0;
  return (uint16_t)(((uint16_t)pub_key[0] << 8) | pub_key[1]);
}

// The point of this slot in a window, from 0 up to window_ms - 1.
inline uint32_t advertPhaseWithin(uint32_t window_ms, const uint8_t* pub_key, size_t key_len) {
  if (window_ms == 0) return 0;
  return (uint32_t)(((uint64_t)advertPhaseFraction(pub_key, key_len) * window_ms) >> 16);
}

// A jitter of a few seconds on each cycle. Two nodes whose adverts collide keep
// the same period forever, so without this they collide again on every cycle.
// The anchor below removes the jitter of the last cycle, so the phase never
// walks away from the key.
#ifndef HYDRA_ADVERT_JITTER_MS
  #define HYDRA_ADVERT_JITTER_MS  30000
#endif

// The delay from now until the next advert of this slot, in milliseconds.
// A result of 0 means that the slot must not advert at all. An interval of 0
// minutes is the only way to get that result, because a live interval always
// gives at least half of itself.
inline uint32_t nextAdvertDelay(uint32_t now, uint8_t interval_mins,
                                const uint8_t* pub_key, size_t key_len,
                                uint32_t jitter_ms) {
  uint32_t interval = (uint32_t)interval_mins * 60000UL;
  if (interval == 0) return 0;
  uint32_t offset = advertPhaseWithin(interval, pub_key, key_len);
  // This puts the advert on the next point of the cycle that belongs to the
  // slot. It does not add the offset to each interval, because that would only
  // make the interval longer. It also pulls the phase back to the key after a
  // jitter. Every term stays below 2 intervals, so nothing here overflows.
  uint32_t phase = now % interval;
  uint32_t delay = (offset + interval - phase) % interval;
  if (delay == 0) delay = interval;   // a slot that is on its phase waits a whole cycle
  // A cycle that is almost over gives a very short delay. Two adverts close
  // together are the fault that this code removes, so wait for the cycle after.
  if (delay < interval / 2) delay += interval;
  return delay + jitter_ms;
}
