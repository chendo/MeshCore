#pragma once

// The rules a slot has to satisfy before it may be enabled, and the node/slot
// CLI namespace split — separated from the firmware so both can be tested on a
// host. Nothing here includes Arduino, Mesh or target headers.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum SlotType : uint8_t {
  SLOT_OFF      = 0,
  SLOT_REPEATER = 1,
  SLOT_CHAT     = 2,
  SLOT_ROOM     = 3,   // room server: own identity, own ACL, own post buffer
};

// Matches NodePrefs::node_name, so slot 0 and slots 1..N have the same ceiling.
#define SLOT_NAME_MAX  32

// Decision G's node-level RAM reserve floor. Enabling a slot must leave at
// least this much heap for BLE connections, LittleFS and packet churn.
#ifndef HYDRA_RAM_RESERVE
  #define HYDRA_RAM_RESERVE  24576
#endif

// ---------------------------------------------------------------- slot naming

// Same character rules as CommonCLI's isValidName(): these break the advert
// listing formats used by the apps.
inline bool slotNameValid(const char* n) {
  if (n == nullptr || n[0] == 0) return false;          // unnamed is not a name
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
  SLOT_ENABLE_RANGE,      // no such slot
  SLOT_ENABLE_SLOT0,      // slot 0 is the repeater and is never toggled
  SLOT_ENABLE_NO_NAME,    // decision H: a nameless identity must not exist
  SLOT_ENABLE_BAD_TYPE,
  SLOT_ENABLE_NO_RAM,     // would breach the reserve floor
  SLOT_ENABLE_FAILED,     // begin() refused for its own reasons
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

// Everything that can be decided without touching the heap or the radio. The
// RAM floor is checked separately, by RamFloor below, because the only honest
// answer to "is there room" is to try.
inline SlotEnableResult slotEnableCheck(int idx, int num_slots, SlotType t, const char* name) {
  if (idx <= 0 || idx >= num_slots) {
    return (idx == 0) ? SLOT_ENABLE_SLOT0 : SLOT_ENABLE_RANGE;
  }
  if (t != SLOT_CHAT && t != SLOT_ROOM) return SLOT_ENABLE_BAD_TYPE;
  if (!slotNameValid(name)) return SLOT_ENABLE_NO_NAME;
  return SLOT_ENABLE_OK;
}

// ------------------------------------------------- decoding a stored record
//
// Safe mode (decision D): every unreadable or unexpected value has to land on
// "slot off", never on a different slot type and never on a running identity
// the config did not actually describe.

// A type byte this build does not implement must not be reinterpreted.
inline SlotType slotTypeFromByte(uint8_t b) {
  return (b == SLOT_CHAT || b == SLOT_ROOM) ? (SlotType)b : SLOT_OFF;
}

// A persisted record only enables a slot if it would also pass the CLI's gate,
// so the no-nameless-identity rule survives a reboot and a corrupt file.
inline SlotType slotTypeFromRecord(uint8_t type_byte, const char* name) {
  SlotType t = slotTypeFromByte(type_byte);
  return slotNameValid(name) ? t : SLOT_OFF;
}

// ------------------------------------------------------------ the RAM reserve
//
// Decision G forbids sizing anything from measured free heap: free-at-boot is
// not free-at-peak and the failure modes are silent. This does not measure.
// It PINS the reserve out of reach and then lets the slot allocate against
// what is left, so a slot that starts has provably left the floor intact —
// and one that cannot is refused now rather than dropping packets at 3am.

class RamFloor {
public:
  typedef void* (*AllocFn)(size_t);
  typedef void  (*FreeFn)(void*);

  RamFloor(size_t bytes, AllocFn a = ::malloc, FreeFn f = ::free)
      : _free_fn(f), _p(nullptr), _wanted(bytes) {
    if (bytes > 0) _p = a(bytes);
  }
  ~RamFloor() { release(); }

  // False means the floor could not even be pinned: the heap is already at or
  // below the reserve and no slot should be started.
  bool held() const { return _wanted == 0 || _p != nullptr; }
  void release() { if (_p) { _free_fn(_p); _p = nullptr; } }

private:
  RamFloor(const RamFloor&);
  RamFloor& operator=(const RamFloor&);
  FreeFn _free_fn;
  void*  _p;
  size_t _wanted;
};

// Largest single block the heap will still hand out, for the `slots` headroom
// column. Reporting only — nothing sizes itself from this.
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
// Decision 8: node-level owns anything backed by one piece of hardware — the
// transceiver, the board, the flash. A slot asking for those is refused rather
// than quietly retuning the antenna under every other slot.

inline bool slotVerbIsNodeLevel(const char* verb) {
  static const char* const kNodeOnly[] = {
    // the one transceiver
    "freq", "bw", "sf", "cr", "tx", "radio", "cad", "int.thresh", "extra.sf",
    "agc.reset.interval", "dutycycle", "af",
    // the one board
    "adc.multiplier", "reboot", "erase", "start", "powersave", "bridge",
    // node-wide state
    "time", "clock", "log", "password", "ver", "neighbors",
    "discover.neighbors",
  };
  if (verb == nullptr || *verb == 0) return false;
  size_t vlen = 0;
  while (verb[vlen] && verb[vlen] != ' ') vlen++;
  for (size_t i = 0; i < sizeof(kNodeOnly) / sizeof(kNodeOnly[0]); i++) {
    size_t klen = strlen(kNodeOnly[i]);
    if (vlen < klen || memcmp(verb, kNodeOnly[i], klen) != 0) continue;
    // exact token, or a dotted child of one ("radio.rxgain" under "radio")
    if (vlen == klen || verb[klen] == '.') return true;
  }
  return false;
}

inline const char* slotNodeLevelError() {
  return "ERR: node-level setting - use it unqualified, not under a slot";
}
