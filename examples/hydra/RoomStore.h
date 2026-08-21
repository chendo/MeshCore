#pragma once

// What a room slot owns beyond its identity: its OWN access list and its own
// post buffer.
//
// PER-ROOM, NOT SHARED (decision H). A room's members are its own business, so
// the ACL is neither the node's nor another slot's. ClientACL persists itself
// to a single fixed path (/s_contacts, slot 0's), so the load/save here go
// through its public API into a slot-scoped file instead. `acl.save()` and
// `acl.load()` must never be called on one of these — they would overwrite the
// repeater's list.
//
// POSTS ARE RAM ONLY (decision G). A flash write per post is the ~1.6 s blocked
// loop and the wear problem the ACL work exists to avoid. Posts are lost on
// reboot, by decision, and MAX_UNSYNCED_POSTS is a delivery window for
// stragglers rather than history — raising it does not give new joiners a
// backlog.
//
// Everything here is heap-allocated when the slot is ENABLED and freed when it
// is disabled, matching DeferredPacketManager: a configured-but-off room costs
// one pointer. That is also what makes the node RAM reserve floor meaningful —
// enabling a room is the moment the heap is actually asked for the memory.

#include <helpers/ClientACL.h>
#include <helpers/IdentityStore.h>
#include <Mesh.h>

#ifndef MAX_UNSYNCED_POSTS
  #define MAX_UNSYNCED_POSTS  32
#endif

#define MAX_POST_TEXT_LEN  (160 - 9)

// Same shape as examples/simple_room_server's PostInfo. Redeclared rather than
// included: that header defines a class MyMesh, and slot 0 already has one.
struct PostInfo {
  mesh::Identity author;
  uint32_t post_timestamp;   // by OUR clock
  char text[MAX_POST_TEXT_LEN + 1];
};

// v1 record: pubkey, permissions, out-path, sync point. The shared secret is
// recomputed on load, so a slot whose private key changed picks up working
// secrets without a stale copy on disk to disagree with.
#define ROOM_ACL_MAGIC_0  'R'
#define ROOM_ACL_MAGIC_1  1

class RoomStore {
public:
  ClientACL acl;
  PostInfo  posts[MAX_UNSYNCED_POSTS];
  int       next_post_idx;
  uint16_t  num_posted;

  RoomStore() : next_post_idx(0), num_posted(0) {
    memset(posts, 0, sizeof(posts));
  }

  static size_t heapCost() { return sizeof(RoomStore); }

  // ---- ACL persistence, in a slot-scoped file -------------------------------

  void load(FILESYSTEM* fs, const mesh::LocalIdentity& self_id, const char* file) {
    if (!fs->exists(file)) return;
  #if defined(RP2040_PLATFORM)
    File f = fs->open(file, "r");
  #else
    File f = fs->open(file);
  #endif
    if (!f) return;
    uint8_t hdr[2];
    if (f.read(hdr, 2) == 2 && hdr[0] == ROOM_ACL_MAGIC_0 && hdr[1] == ROOM_ACL_MAGIC_1) {
      for (;;) {
        uint8_t pub[PUB_KEY_SIZE], perms, path_len, path[MAX_PATH_SIZE];
        uint32_t sync_since;
        bool ok = (f.read(pub, PUB_KEY_SIZE) == PUB_KEY_SIZE);
        ok = ok && (f.read(&perms, 1) == 1);
        ok = ok && (f.read(&path_len, 1) == 1);
        ok = ok && (f.read(path, MAX_PATH_SIZE) == MAX_PATH_SIZE);
        ok = ok && (f.read((uint8_t*)&sync_since, 4) == 4);
        if (!ok) break;   // EOF or a short tail; keep what was read
        // applyPermissions() is the only public route that also derives the
        // shared secret, which is why the record does not store one.
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
    uint8_t hdr[2] = { ROOM_ACL_MAGIC_0, ROOM_ACL_MAGIC_1 };
    f.write(hdr, 2);
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
