// Room identity = stock examples/simple_room_server/MyMesh.cpp, compiled here
// under the name RoomMesh. The example source is NOT modified.

// keep this stock example headless even if the env defines the web panel
#ifdef WITH_WEB_PANEL
#undef WITH_WEB_PANEL
#endif
#ifdef WITH_MQTT_UPLINK
#undef WITH_MQTT_UPLINK
#endif

#define MyMesh RoomMesh
#include "../simple_room_server/MyMesh.cpp"
#undef MyMesh

// The room's stored posts (RAM ring) are private with no CLI to read them,
// and the stock example must stay unmodified. Explicit template instantiation
// is allowed to name private members ([temp.spec] exempts it from access
// checks), so this standard-compliant backdoor exports member pointers for
// roomGetPostsJson() below without touching the example or its ABI.
namespace roomspy {
  template <typename Tag, typename Tag::type M>
  struct Rob { friend typename Tag::type get(Tag) { return M; } };

  struct PostsTag { typedef PostInfo (RoomMesh::*type)[MAX_UNSYNCED_POSTS]; friend type get(PostsTag); };
  template struct Rob<PostsTag, &RoomMesh::posts>;

  struct IdxTag { typedef int RoomMesh::*type; friend type get(IdxTag); };
  template struct Rob<IdxTag, &RoomMesh::next_post_idx>;
}

#include "identity_module.h"
#include "multi_web.h"
#include "identity_backup.h"
#include <target.h>
#include <helpers/ArduinoHelpers.h>

static RoomMesh*        g_room = nullptr;
static StdRNG           room_rng;
static SimpleMeshTables room_tables;

static void room_setup(MultiFS* fs, mesh::Radio* port) {
  room_rng.begin(radio_driver.getRngSeed());
  g_room = new RoomMesh(board, *port, *new ArduinoMillis(), room_rng, rtc_clock, room_tables);

  if (!multiIdLoad("room", fs, g_room->self_id)) {   // FS copy, else NVS mirror
    g_room->self_id = radio_new_identity();
    IdentityStore store(*fs, "/identity");
    store.begin();
    store.save("_main", g_room->self_id);
    multiIdImportSaveMirror("room", g_room->self_id);
    Serial.println("[room] NEW identity created (no saved copy found)");
  }
  Serial.print("[room] ID: ");
  mesh::Utils::printHex(Serial, g_room->self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  g_room->begin(fs);
}

static void room_loop() { if (g_room) g_room->loop(); }

static void room_cmd(const char* c, char* r, size_t n) {
  if (!g_room) { if (n) r[0] = 0; return; }
  char buf[160];
  strncpy(buf, c, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  g_room->handleCommand(0, buf, r);
}

static void room_pk(uint8_t out[32]) { if (g_room) memcpy(out, g_room->self_id.pub_key, PUB_KEY_SIZE); }

static uint32_t room_pool_full() { return g_room ? g_room->getNumRxPoolFull() : 0; }

IdentityModule room_module = { "room", room_setup, room_loop, room_cmd, room_pk, room_pool_full };

// Stored posts (newest last), as JSON for the web panel. Reads the room's
// cyclic RAM queue directly (see the access-override note above the include).
static int roomFillPostsJson(char* out, size_t cap) {
  if (g_room == nullptr || out == nullptr || cap < 3) { if (out && cap) out[0] = 0; return 0; }
  size_t o = 0;
  out[o++] = '[';
  bool first = true;
  // posts[] is a cyclic queue; next_post_idx points at the oldest slot
  PostInfo (&posts)[MAX_UNSYNCED_POSTS] = g_room->*get(roomspy::PostsTag());   // ADL finds the friend
  int next_idx = g_room->*get(roomspy::IdxTag());
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    PostInfo& p = posts[(next_idx + k) % MAX_UNSYNCED_POSTS];
    if (p.post_timestamp == 0) continue;
    char author[13];
    mesh::Utils::toHex(author, p.author.pub_key, 6);
    char esc_text[MAX_POST_TEXT_LEN * 2 + 2];
    size_t e = 0;
    for (const char* c = p.text; *c && e < sizeof(esc_text) - 2; c++) {
      if (*c == '"' || *c == '\\') esc_text[e++] = '\\';
      if ((uint8_t)*c >= 0x20) esc_text[e++] = *c; else esc_text[e++] = ' ';
    }
    esc_text[e] = 0;
    int need = snprintf(nullptr, 0, "%s{\"t\":%lu,\"a\":\"%s\",\"x\":\"%s\"}",
                        first ? "" : ",", (unsigned long)p.post_timestamp, author, esc_text);
    if (o + need + 2 >= cap) break;
    o += snprintf(out + o, cap - o, "%s{\"t\":%lu,\"a\":\"%s\",\"x\":\"%s\"}",
                  first ? "" : ",", (unsigned long)p.post_timestamp, author, esc_text);
    first = false;
  }
  out[o++] = ']';
  out[o] = 0;
  return (int)o;
}

// The panel polls this from the HTTPS task while the loop task is receiving
// posts into the very ring being walked — next_post_idx advancing mid-read
// duplicates or skips entries, and a post being overwritten mid-copy yields a
// half-written string. Run the read on the loop task instead.
//
// Request and scratch are one heap block owned by the call, because if the loop
// task is too slow to answer it will still run the read afterwards. On that
// path the block is deliberately leaked rather than freed underneath it.
namespace {
  struct PostsReq { size_t cap; int len; char buf[1]; };
  void roomPostsOnLoop(void* p) {
    PostsReq* r = (PostsReq*)p;
    r->len = roomFillPostsJson(r->buf, r->cap);
  }
}

int roomGetPostsJson(char* out, size_t cap) {
  if (out == nullptr || cap < 3) return 0;
  if (multiOnLoopTask()) return roomFillPostsJson(out, cap);

  PostsReq* r = (PostsReq*)malloc(sizeof(PostsReq) + cap);
  if (r == nullptr) { snprintf(out, cap, "[]"); return 2; }
  r->cap = cap; r->len = 0; r->buf[0] = 0;

  if (!multiRunInLoop(roomPostsOnLoop, r, 4000)) {
    snprintf(out, cap, "[]");     // r is intentionally NOT freed — still queued
    return 2;
  }
  int n = r->len;
  memcpy(out, r->buf, (size_t)n + 1);
  free(r);
  return n;
}
