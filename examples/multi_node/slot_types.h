#pragma once

// What an optional identity slot IS, rather than assuming it is a chat client.
//
// Slots began as extra companion identities, so everything about them — the NVS
// key, the filesystem directory, the module name — says "chat". They are really
// just spare radio ports with storage attached, and any stock MeshCore role can
// sit on one. This header is the type layer that lets a slot be a room server
// as easily as a chat client.
//
// STORAGE NAMES DO NOT CHANGE WITH TYPE. A slot keeps /fs/chatN and the NVS
// identity-mirror key "chatN" whatever role it runs, because those names are
// what a keypair is filed under, and this node has lost identities before by
// letting a path move out from under one. A slot's type is a property of what
// it does, not of who it is: retyping a slot keeps its key, which is also the
// useful behaviour (same node, different role).

#include <Arduino.h>
#include "identity_module.h"

enum SlotType {
  SLOT_OFF  = 0,
  SLOT_CHAT = 1,   // companion / phone-app identity, own TCP port
  SLOT_ROOM = 2,   // room server, managed over CLI + panel
};
#define SLOT_TYPE_COUNT 3

// "off" | "chat" | "room"; nullptr for an unknown value
const char* slotTypeName(SlotType t);
// parses the names above; returns false if it is not one of them
bool        slotTypeParse(const char* s, SlotType* out);

// Persisted in NVS namespace "multislots". Reads the legacy per-slot bool
// ("chatN" = enabled) when no explicit type is stored, so slots configured
// before types existed keep running as chat identities untouched.
SlotType slotTypeGet(int i);
void     slotTypeSet(int i, SlotType t);

// The module that should drive slot i, chosen by its configured type, or
// nullptr when the slot is off. Instances are owned by the per-type wrappers
// (wrap_slots.cpp for chat, wrap_room.cpp for rooms).
IdentityModule* slotModule(int i);
// Where slot i's storage lives — the same path regardless of type.
const char*     slotFsDir(int i);
// Short label for listings: "chat1 (room)" etc.
int             slotDescribe(int i, char* out, size_t cap);
