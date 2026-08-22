#pragma once

// The part of the room protocol that has no radio in it.
//
// This file is the delivery buffer and the choice of the next post for one
// client. It is a port of the same logic in examples/simple_room_server. It
// keeps that behaviour exactly. It moves the logic here because a host test can
// then run it: there is no Arduino header, no mesh type and no transceiver
// below this line. RoomMesh.h holds the part that needs a radio.
//
// THIS IS A DELIVERY BUFFER. IT IS NOT A HISTORY. The ring holds
// MAX_UNSYNCED_POSTS posts. The node pushes a post to every client whose
// `sync_since` is older than that post. A post keeps its slot after the last
// client synced past it, until a newer post overwrites the slot. So a larger
// MAX_UNSYNCED_POSTS widens the window for a client that is late. It does NOT
// give a new member a backlog, and a client that is away for longer than the
// depth of the ring loses those posts for good. This is the design of upstream.
// Read this paragraph before you tune the number.

#include <MeshCore.h>   // PUB_KEY_SIZE
#include <stdint.h>
#include <string.h>

#ifndef MAX_UNSYNCED_POSTS
  #define MAX_UNSYNCED_POSTS  32
#endif

#define MAX_POST_TEXT_LEN  (160 - 9)

// A post waits this many seconds before the node offers it to anybody. The
// author gets its ACK first, and a client that posts and then reads does not
// race its own message.
#ifndef POST_SYNC_DELAY_SECS
  #define POST_SYNC_DELAY_SECS  6
#endif

// After this many pushes with no ACK, the node stops pushing to that client.
// The client resumes it by talking: a login, a post, a request or a keep-alive
// all set push_failures back to 0.
#define ROOM_MAX_PUSH_FAILURES  3

// The author is a raw public key, and not a mesh::Identity. The 32 bytes are
// the same. The raw array keeps this file free of mesh headers, so the host
// tests build the real struct and not a copy of it.
struct PostInfo {
  uint8_t  author_key[PUB_KEY_SIZE];
  uint32_t post_timestamp;   // by OUR clock
  char     text[MAX_POST_TEXT_LEN + 1];
};

class PostRing {
public:
  PostInfo posts[MAX_UNSYNCED_POSTS];
  int      next_post_idx;    // the oldest slot, and the next slot to overwrite
  uint16_t num_posted;       // posts ever stored. This is NOT the number held.
  uint16_t num_post_pushes;  // pushes ever sent, retries included

  PostRing() { reset(); }

  void reset() {
    memset(posts, 0, sizeof(posts));
    next_post_idx = 0;
    num_posted = 0;
    num_post_pushes = 0;
  }

  // The new post takes the oldest slot. What was in that slot is gone, even if
  // a client never received it.
  void add(const uint8_t* author_key, const char* text, uint32_t timestamp) {
    PostInfo& p = posts[next_post_idx];
    memcpy(p.author_key, author_key, PUB_KEY_SIZE);
    strncpy(p.text, text, MAX_POST_TEXT_LEN);
    p.text[MAX_POST_TEXT_LEN] = 0;
    p.post_timestamp = timestamp;
    next_post_idx = (next_post_idx + 1) % MAX_UNSYNCED_POSTS;
    num_posted++;
  }

  // The index of the next post for this client, or -1 for none. The scan starts
  // at the oldest slot, so a client receives posts in the order of the ring.
  // An empty slot has a timestamp of 0 and never matches.
  int nextForClient(const uint8_t* client_key, uint32_t sync_since, uint32_t now) const {
    int idx = next_post_idx;
    for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
      const PostInfo* p = &posts[idx];
      if (now >= p->post_timestamp + POST_SYNC_DELAY_SECS
          && p->post_timestamp > sync_since
          && !isAuthor(*p, client_key)) {
        return idx;
      }
      idx = (idx + 1) % MAX_UNSYNCED_POSTS;
    }
    return -1;
  }

  // What the node tells a client is waiting for it. This count has no delay
  // gate, so it also counts a post that is too new to push. That is the
  // behaviour of upstream, and the client uses the number as a hint.
  uint8_t unsyncedCount(const uint8_t* client_key, uint32_t sync_since) const {
    uint8_t count = 0;
    for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
      const PostInfo& p = posts[k];
      if (p.post_timestamp > sync_since && !isAuthor(p, client_key)) count++;
    }
    return count;
  }

  // A client never receives its own post back. It already has the text, and its
  // own ACK told it that the room holds the post.
  static bool isAuthor(const PostInfo& p, const uint8_t* client_key) {
    return memcmp(p.author_key, client_key, PUB_KEY_SIZE) == 0;
  }
};

// The round-robin skips a client that still owes an ACK, a client that never
// showed activity, and a client that used all of its attempts.
inline bool roomClientMayPush(uint32_t pending_ack, uint32_t last_activity, uint8_t push_failures) {
  return pending_ack == 0 && last_activity != 0 && push_failures < ROOM_MAX_PUSH_FAILURES;
}

// The push got no ACK in time. The caller counts one failure and clears
// pending_ack, which lets the round-robin send the same post again.
//
// The comparison is signed, so a deadline just after the wrap of millis() is
// still in the future. The types are 32 bits and not `long`, because `long` is
// 64 bits on the host that runs the tests and 32 bits on every target board.
// Fixed widths make the wrap the same in both places, so a test can prove it.
inline bool roomPushAckExpired(uint32_t pending_ack, uint32_t now_millis,
                               uint32_t ack_timeout) {
  return pending_ack != 0 && (int32_t)(now_millis - ack_timeout) >= 0;
}
