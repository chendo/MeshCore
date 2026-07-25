// Second room slot = another instance of the stock simple_room_server, with
// its own identity, storage subdirectory (/fs/room2) and radio port. The
// example source is NOT modified. This slot is compiled in but only started
// when the operator enables it (multisys NVS flag, read at boot — enabling or
// disabling from the panel takes effect after a reboot, because radio ports
// must be registered before the shared-radio frame fan-out starts).

// keep this stock example headless even if the env defines the web panel
#ifdef WITH_WEB_PANEL
#undef WITH_WEB_PANEL
#endif
#ifdef WITH_MQTT_UPLINK
#undef WITH_MQTT_UPLINK
#endif

#define MyMesh RoomMesh2
#include "../simple_room_server/MyMesh.cpp"
#undef MyMesh

#include "identity_module.h"
#include <target.h>
#include <helpers/ArduinoHelpers.h>

static RoomMesh2*       g_room2 = nullptr;
static StdRNG           room2_rng;
static SimpleMeshTables room2_tables;

static void room2_setup(MultiFS* fs, mesh::Radio* port) {
  room2_rng.begin(radio_driver.getRngSeed());
  g_room2 = new RoomMesh2(board, *port, *new ArduinoMillis(), room2_rng, rtc_clock, room2_tables);

  IdentityStore store(*fs, "/identity");
  store.begin();
  if (!store.load("_main", g_room2->self_id)) {
    g_room2->self_id = radio_new_identity();
    store.save("_main", g_room2->self_id);
  }
  Serial.print("[room2] ID: ");
  mesh::Utils::printHex(Serial, g_room2->self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  g_room2->begin(fs);
}

static void room2_loop() { if (g_room2) g_room2->loop(); }

static void room2_cmd(const char* c, char* r, size_t n) {
  if (!g_room2) { if (n) { strncpy(r, "room2 is disabled (enable in Rooms, then reboot)", n - 1); r[n - 1] = 0; } return; }
  char buf[160];
  strncpy(buf, c, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  g_room2->handleCommand(0, buf, r);
}

static void room2_pk(uint8_t out[32]) { if (g_room2) memcpy(out, g_room2->self_id.pub_key, PUB_KEY_SIZE); }

IdentityModule room2_module = { "room2", room2_setup, room2_loop, room2_cmd, room2_pk };
