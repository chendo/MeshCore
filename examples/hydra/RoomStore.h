#pragma once

// A room slot owns two things in addition to its identity. It owns its OWN
// access list and its own post buffer.
//
// ONE PER ROOM, NOT SHARED (decision H). The members of a room are the business
// of that room. So the ACL does not belong to the node, and it does not belong
// to another slot. ClientACL saves itself to one fixed path, /s_contacts, which
// is the path of slot 0. The load and save functions here therefore use the
// public API of ClientACL and write to a slot-scoped file. Never call
// `acl.save()` or `acl.load()` on one of these. They would overwrite the list
// of the repeater.
//
// POSTS ARE IN RAM ONLY (decision G). One flash write for each post gives the
// ~1.6 s block of the loop and the wear problem that the ACL work exists to
// avoid. By decision, a reboot loses the posts. MAX_UNSYNCED_POSTS is a
// delivery buffer for late clients. It is not a history. A larger value does
// not give a new member a backlog. RoomSync.h says why.
//
// This object also holds the working state of the protocol: the reply buffer,
// the peer-match table, the round-robin cursor and the telemetry encoder. They
// live here and not in the mesh object for two reasons. A chat slot then pays
// nothing for them. And RoomStore::heapCost() then reports the true cost of a
// room to the node RAM reserve.
//
// The code allocates everything here on the heap when you ENABLE the slot. It
// frees it when you disable the slot. This matches DeferredPacketManager. A
// room that is configured but off costs one pointer. This is also what makes
// the node RAM reserve floor useful. To enable a room is the moment when the
// code asks the heap for the memory.

#include "RoomSync.h"
#include <helpers/ClientACL.h>
#include <helpers/IdentityStore.h>
#include <helpers/SensorManager.h>
#include <Mesh.h>

// A v1 record holds the pubkey, the permissions, the out-path and the sync
// point. The code calculates the shared secret again at each load. So a slot
// whose private key changed gets secrets that work. There is no old copy on the
// disk to disagree with them.
//
// v2 adds the settings of the room before the records: the room password and
// the read-only flag. A v1 file still loads, and it gives the default for both.
#define ROOM_ACL_MAGIC_0  'R'
#define ROOM_ACL_MAGIC_1  1
#define ROOM_ACL_MAGIC_2  2

#define ROOM_PASSWORD_LEN  16

class RoomStore {
public:
  ClientACL acl;
  PostRing  ring;

  // ---- the settings of the room ---------------------------------------------
  // A client that sends this password joins with read and write rights. An
  // EMPTY password matches the empty string that a client sends when it has no
  // password. So a room with no password set is OPEN. That is the behaviour of
  // examples/simple_room_server, and this port keeps it.
  char    guest_password[ROOM_PASSWORD_LEN];
  uint8_t allow_read_only;   // admit a wrong password as a reader

  // ---- the working state of the protocol ------------------------------------
  uint8_t  reply_data[MAX_PACKET_PAYLOAD];
  int      matching_peer_indexes[MAX_CLIENTS];
  unsigned long next_push;
  int      next_client_idx;   // the round-robin cursor over the ACL
  bool     acl_dirty;         // the slot turns this into one delayed flash write
  CayenneLPP telemetry;

  RoomStore() : allow_read_only(0), next_push(0), next_client_idx(0), acl_dirty(false),
                telemetry(MAX_PACKET_PAYLOAD - 4) {
    guest_password[0] = 0;
    memset(matching_peer_indexes, 0, sizeof(matching_peer_indexes));
  }

  static size_t heapCost() { return sizeof(RoomStore); }

  // ---- how the code stores the ACL, in a slot-scoped file -------------------

  void load(FILESYSTEM* fs, const mesh::LocalIdentity& self_id, const char* file) {
    if (!fs->exists(file)) return;
  #if defined(RP2040_PLATFORM)
    File f = fs->open(file, "r");
  #else
    File f = fs->open(file);
  #endif
    if (!f) return;
    uint8_t hdr[2];
    if (f.read(hdr, 2) == 2 && hdr[0] == ROOM_ACL_MAGIC_0
        && (hdr[1] == ROOM_ACL_MAGIC_1 || hdr[1] == ROOM_ACL_MAGIC_2)) {
      if (hdr[1] >= ROOM_ACL_MAGIC_2) {
        if (f.read((uint8_t*)guest_password, ROOM_PASSWORD_LEN) != ROOM_PASSWORD_LEN) {
          guest_password[0] = 0;
        }
        guest_password[ROOM_PASSWORD_LEN - 1] = 0;
        if (f.read(&allow_read_only, 1) != 1) allow_read_only = 0;
      }
      for (;;) {
        uint8_t pub[PUB_KEY_SIZE], perms, path_len, path[MAX_PATH_SIZE];
        uint32_t sync_since;
        bool ok = (f.read(pub, PUB_KEY_SIZE) == PUB_KEY_SIZE);
        ok = ok && (f.read(&perms, 1) == 1);
        ok = ok && (f.read(&path_len, 1) == 1);
        ok = ok && (f.read(path, MAX_PATH_SIZE) == MAX_PATH_SIZE);
        ok = ok && (f.read((uint8_t*)&sync_since, 4) == 4);
        if (!ok) break;   // the end of the file, or a short tail. Keep what we read.
        // applyPermissions() is the only public function that also calculates
        // the shared secret. This is why the record does not store one.
        if (perms == 0 || !acl.applyPermissions(self_id, pub, PUB_KEY_SIZE, perms)) continue;
        ClientInfo* c = acl.getClient(pub, PUB_KEY_SIZE);
        if (c) {
          c->out_path_len = path_len;
          memcpy(c->out_path, path, MAX_PATH_SIZE);
          c->extra.room.sync_since = sync_since;
        }
      }
    }
    f.close();
  }

  bool save(FILESYSTEM* fs, const char* file) {
  #if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    fs->remove(file);
    File f = fs->open(file, FILE_O_WRITE);
  #elif defined(RP2040_PLATFORM)
    File f = fs->open(file, "w");
  #else
    File f = fs->open(file, "w", true);
  #endif
    if (!f) return false;
    uint8_t hdr[2] = { ROOM_ACL_MAGIC_0, ROOM_ACL_MAGIC_2 };
    f.write(hdr, 2);
    f.write((const uint8_t*)guest_password, ROOM_PASSWORD_LEN);
    f.write(&allow_read_only, 1);
    for (int i = 0; i < acl.getNumClients(); i++) {
      ClientInfo* c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;   // deleted / guest
      f.write(c->id.pub_key, PUB_KEY_SIZE);
      f.write(&c->permissions, 1);
      f.write(&c->out_path_len, 1);
      f.write(c->out_path, MAX_PATH_SIZE);
      f.write((uint8_t*)&c->extra.room.sync_since, 4);
    }
    f.close();
    return true;
  }
};
