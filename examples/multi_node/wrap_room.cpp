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

// Instance 0 is the fixed "room" role; 1..MULTI_MAX_CHAT_SLOTS back optional
// slots configured as rooms. Everything here is per-object — the stock example
// declares no file-scope state at all, only MyMesh:: members — so N instances
// need nothing but N sets of collaborators. They are heap-allocated at setup,
// so an unused instance costs one null pointer.
#define ROOM_INSTANCES (1 + MULTI_MAX_CHAT_SLOTS)

struct RoomInst {
  RoomMesh*         mesh;
  StdRNG            rng;
  SimpleMeshTables  tables;
  char              name[8];    // "room" | "chat2" ... — the IDENTITY name, see slot_types.h
};
static RoomInst s_rooms[ROOM_INSTANCES];
// Instance 0 keeps its own named global: main.cpp takes its address in a static
// initialiser, so it needs a stable symbol, not an array slot handed out later.
IdentityModule room_module;
static IdentityModule s_slot_room_modules[ROOM_INSTANCES - 1];
static IdentityModule* roomModuleSlot(int i) {
  return (i == 0) ? &room_module : &s_slot_room_modules[i - 1];
}

template <int I>
static void room_setup(MultiFS* fs, mesh::Radio* port) {
  RoomInst& r = s_rooms[I];
  r.rng.begin(radio_driver.getRngSeed());
  r.mesh = new RoomMesh(board, *port, *new ArduinoMillis(), r.rng, rtc_clock, r.tables);

  if (!multiIdLoad(r.name, fs, r.mesh->self_id)) {   // FS copy, else NVS mirror
    r.mesh->self_id = radio_new_identity();
    IdentityStore store(*fs, "/identity");
    store.begin();
    store.save("_main", r.mesh->self_id);
    multiIdImportSaveMirror(r.name, r.mesh->self_id);
    Serial.printf("[%s] NEW room identity created (no saved copy found)\n", r.name);
  }
  Serial.printf("[%s] room ID: ", r.name);
  mesh::Utils::printHex(Serial, r.mesh->self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  r.mesh->begin(fs);
}

template <int I>
static void room_loop() { if (s_rooms[I].mesh) s_rooms[I].mesh->loop(); }

// Routes straight into the stock room CLI, which is where `password`,
// `set name`, advert intervals and the rest already live — so managing an
// extra room needs no new command surface, only a way to address it.
template <int I>
static void room_cmd(const char* c, char* r, size_t n) {
  if (!s_rooms[I].mesh) { if (n) r[0] = 0; return; }
  char buf[160];
  strncpy(buf, c, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  s_rooms[I].mesh->handleCommand(0, buf, r);
}

template <int I>
static void room_pk(uint8_t out[32]) {
  if (s_rooms[I].mesh) memcpy(out, s_rooms[I].mesh->self_id.pub_key, PUB_KEY_SIZE);
}

template <int I>
static uint32_t room_pool_full() {
  return s_rooms[I].mesh ? s_rooms[I].mesh->getNumRxPoolFull() : 0;
}

void roomInstancesInit(const char* const* slot_names) {
  strncpy(s_rooms[0].name, "room", sizeof(s_rooms[0].name) - 1);
  for (int i = 1; i < ROOM_INSTANCES; i++) {
    strncpy(s_rooms[i].name, slot_names[i - 1], sizeof(s_rooms[i].name) - 1);
    s_rooms[i].name[sizeof(s_rooms[i].name) - 1] = 0;
  }
  for (int i = 0; i < ROOM_INSTANCES; i++) {
    s_rooms[i].mesh = nullptr;
    roomModuleSlot(i)->name = s_rooms[i].name;
  }
  #define WIRE_ROOM(N) { IdentityModule* m = roomModuleSlot(N); \
    m->setup = &room_setup<N>; m->loop = &room_loop<N>; \
    m->run_command = &room_cmd<N>; m->get_pubkey = &room_pk<N>; \
    m->rx_pool_full = &room_pool_full<N>; }
  WIRE_ROOM(0) WIRE_ROOM(1) WIRE_ROOM(2) WIRE_ROOM(3) WIRE_ROOM(4) WIRE_ROOM(5)
  #undef WIRE_ROOM
}

// instance 0 = the fixed role; slot k (0-based) uses instance k+1
IdentityModule* roomInstanceModule(int i) {
  return (i >= 0 && i < ROOM_INSTANCES) ? roomModuleSlot(i) : nullptr;
}
bool roomInstanceRunning(int i) {
  return i >= 0 && i < ROOM_INSTANCES && s_rooms[i].mesh != nullptr;
}

// Stored posts (newest last), as JSON for the web panel. Reads the room's
// cyclic RAM queue directly (see the access-override note above the include).
static int roomFillPostsJson(int inst, char* out, size_t cap) {
  RoomMesh* g_room = (inst >= 0 && inst < ROOM_INSTANCES) ? s_rooms[inst].mesh : nullptr;
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
  struct PostsReq { int inst; size_t cap; int len; char buf[1]; };
  void roomPostsOnLoop(void* p) {
    PostsReq* r = (PostsReq*)p;
    r->len = roomFillPostsJson(r->inst, r->buf, r->cap);
  }
}

int roomGetPostsJson(int inst, char* out, size_t cap) {
  if (out == nullptr || cap < 3) return 0;
  if (multiOnLoopTask()) return roomFillPostsJson(inst, out, cap);

  PostsReq* r = (PostsReq*)malloc(sizeof(PostsReq) + cap);
  if (r == nullptr) { snprintf(out, cap, "[]"); return 2; }
  r->inst = inst; r->cap = cap; r->len = 0; r->buf[0] = 0;

  if (!multiRunInLoop(roomPostsOnLoop, r, 4000)) {
    snprintf(out, cap, "[]");     // r is intentionally NOT freed — still queued
    return 2;
  }
  int n = r->len;
  memcpy(out, r->buf, (size_t)n + 1);
  free(r);
  return n;
}
