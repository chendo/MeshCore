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

#include "identity_module.h"
#include <target.h>
#include <helpers/ArduinoHelpers.h>

static RoomMesh*        g_room = nullptr;
static StdRNG           room_rng;
static SimpleMeshTables room_tables;

static void room_setup(MultiFS* fs, mesh::Radio* port) {
  room_rng.begin(radio_driver.getRngSeed());
  g_room = new RoomMesh(board, *port, *new ArduinoMillis(), room_rng, rtc_clock, room_tables);

  IdentityStore store(*fs, "/identity");
  store.begin();
  if (!store.load("_main", g_room->self_id)) {
    g_room->self_id = radio_new_identity();
    store.save("_main", g_room->self_id);
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

IdentityModule room_module = { "room", room_setup, room_loop, room_cmd, room_pk };
