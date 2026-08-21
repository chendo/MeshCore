#include "slot_types.h"
#include "multi_web.h"
#include <Preferences.h>

// Canonical per-slot names. These are IDENTITY names — the NVS mirror key and
// the filesystem directory are both derived from them — so they are fixed at
// "chatN" for all time regardless of what role the slot runs. See the note in
// slot_types.h: a slot's type may change, its filing must not.
static char s_names[MULTI_MAX_CHAT_SLOTS][8];
static char s_dirs[MULTI_MAX_CHAT_SLOTS][12];
static bool s_inited = false;

static void slotNamesInit() {
  if (s_inited) return;
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    snprintf(s_names[i], sizeof(s_names[i]), "chat%d", i + 1);
    snprintf(s_dirs[i], sizeof(s_dirs[i]), "/fs/chat%d", i + 1);
  }
  s_inited = true;
}

const char* slotTypeName(SlotType t) {
  switch (t) {
    case SLOT_OFF:  return "off";
    case SLOT_CHAT: return "chat";
    case SLOT_ROOM: return "room";
  }
  return nullptr;
}

bool slotTypeParse(const char* s, SlotType* out) {
  if (s == nullptr || out == nullptr) return false;
  if (strcmp(s, "off")  == 0) { *out = SLOT_OFF;  return true; }
  if (strcmp(s, "chat") == 0) { *out = SLOT_CHAT; return true; }
  if (strcmp(s, "room") == 0) { *out = SLOT_ROOM; return true; }
  return false;
}

SlotType slotTypeGet(int i) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return SLOT_OFF;
  char tkey[8], bkey[8];
  snprintf(tkey, sizeof(tkey), "t%d", i + 1);
  snprintf(bkey, sizeof(bkey), "chat%d", i + 1);
  Preferences p; p.begin("multislots", true);
  int t = (int)p.getUChar(tkey, 0xFF);
  // Migration: before types existed a slot was just a bool. An unset type with
  // the legacy flag on means "this was a chat slot" — read it that way rather
  // than silently switching a running identity off.
  bool legacy = p.getBool(bkey, false);
  p.end();
  if (t == 0xFF) return legacy ? SLOT_CHAT : SLOT_OFF;
  if (t < 0 || t >= SLOT_TYPE_COUNT) return SLOT_OFF;
  return (SlotType)t;
}

void slotTypeSet(int i, SlotType t) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return;
  char tkey[8], bkey[8];
  snprintf(tkey, sizeof(tkey), "t%d", i + 1);
  snprintf(bkey, sizeof(bkey), "chat%d", i + 1);
  Preferences p; p.begin("multislots", false);
  p.putUChar(tkey, (uint8_t)t);
  // keep the legacy flag consistent so anything still reading it agrees
  p.putBool(bkey, t == SLOT_CHAT);
  p.end();
}

IdentityModule* slotModule(int i) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return nullptr;
  switch (slotTypeGet(i)) {
    case SLOT_CHAT: return multiChatSlotModule(i);
    case SLOT_ROOM: return roomInstanceModule(i + 1);   // instance 0 is the fixed room
    case SLOT_OFF:  break;
  }
  return nullptr;
}

const char* slotFsDir(int i) {
  slotNamesInit();
  return (i >= 0 && i < MULTI_MAX_CHAT_SLOTS) ? s_dirs[i] : "";
}

const char* const* slotNames() {
  slotNamesInit();
  static const char* ptrs[MULTI_MAX_CHAT_SLOTS];
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) ptrs[i] = s_names[i];
  return ptrs;
}

int slotDescribe(int i, char* out, size_t cap) {
  slotNamesInit();
  if (out == nullptr || cap == 0) return 0;
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) { out[0] = 0; return 0; }
  SlotType t = slotTypeGet(i);
  bool running = false;
  if (t == SLOT_CHAT) running = multiChatSlotRunning(i);
  else if (t == SLOT_ROOM) running = roomInstanceRunning(i + 1);
  if (t == SLOT_CHAT) {
    return snprintf(out, cap, "slot %d (%s) type=chat%s tcp/%d", i + 1, s_names[i],
                    running ? " running" : " stopped", multiChatSlotPort(i));
  }
  if (t == SLOT_ROOM) {
    return snprintf(out, cap, "slot %d (%s) type=room%s", i + 1, s_names[i],
                    running ? " running" : " stopped");
  }
  return snprintf(out, cap, "slot %d (%s) type=off", i + 1, s_names[i]);
}
