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
// delivery window for late clients. It is not a history. A larger value does
// not give a new member a backlog.
//
// The code allocates everything here on the heap when you ENABLE the slot. It
// frees it when you disable the slot. This matches DeferredPacketManager. A
// room that is configured but off costs one pointer. This is also what makes
// the node RAM reserve floor useful. To enable a room is the moment when the
// code asks the heap for the memory.

#include <helpers/ClientACL.h>
#include <helpers/IdentityStore.h>
#include <Mesh.h>

#ifndef MAX_UNSYNCED_POSTS
  #define MAX_UNSYNCED_POSTS  32
#endif

#define MAX_POST_TEXT_LEN  (160 - 9)

// This has the same shape as PostInfo in examples/simple_room_server. We
// declare it again and do not include that header. That header defines a class
// MyMesh, and slot 0 already has one.
struct PostInfo {
  mesh::Identity author;
  uint32_t post_timestamp;   // by OUR clock
  char text[MAX_POST_TEXT_LEN + 1];
};

// A v1 record holds the pubkey, the permissions, the out-path and the sync
// point. The code calculates the shared secret again at each load. So a slot
// whose private key changed gets secrets that work. There is no old copy on the
// disk to disagree with them.
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
    if (f.read(hdr, 2) == 2 && hdr[0] == ROOM_ACL_MAGIC_0 && hdr[1] == ROOM_ACL_MAGIC_1) {
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
