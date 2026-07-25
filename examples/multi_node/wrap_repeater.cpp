// Repeater identity = stock examples/simple_repeater/MyMesh.cpp, compiled here
// under the name RepeaterMesh so three example meshes can coexist in one binary.
// The example source is NOT modified.

#include <helpers/ArchiveStorage.h>

// keep this stock example headless even if the env defines the web panel
#ifdef WITH_WEB_PANEL
#undef WITH_WEB_PANEL
#endif
#ifdef WITH_MQTT_UPLINK
#undef WITH_MQTT_UPLINK
#endif

#define MyMesh RepeaterMesh
#include "../simple_repeater/MyMesh.cpp"
#undef MyMesh

#include "identity_module.h"
#include "identity_backup.h"
#include <target.h>
#include <helpers/ArduinoHelpers.h>

static RepeaterMesh* g_rep = nullptr;
static StdRNG        rep_rng;
static SimpleMeshTables rep_tables;

static void rep_setup(MultiFS* fs, mesh::Radio* port) {
  rep_rng.begin(radio_driver.getRngSeed());
  g_rep = new RepeaterMesh(board, *port, *new ArduinoMillis(), rep_rng, rtc_clock, rep_tables);

  if (!multiIdLoad("repeater", fs, g_rep->self_id)) {   // FS copy, else NVS mirror
    g_rep->self_id = radio_new_identity();
    IdentityStore store(*fs, "/identity");
    store.begin();
    store.save("_main", g_rep->self_id);
    multiIdImportSaveMirror("repeater", g_rep->self_id);
    Serial.println("[repeater] NEW identity created (no saved copy found)");
  }
  Serial.print("[repeater] ID: ");
  mesh::Utils::printHex(Serial, g_rep->self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  g_rep->begin(fs, nullptr);   // no archive partition in the multi layout
}

static void rep_loop() { if (g_rep) g_rep->loop(); }

static void rep_cmd(const char* c, char* r, size_t n) {
  if (!g_rep) { if (n) r[0] = 0; return; }
  char buf[160];
  strncpy(buf, c, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  g_rep->handleCommand(0, buf, r);
}

static void rep_pk(uint8_t out[32]) { if (g_rep) memcpy(out, g_rep->self_id.pub_key, PUB_KEY_SIZE); }

IdentityModule repeater_module = { "repeater", rep_setup, rep_loop, rep_cmd, rep_pk };
