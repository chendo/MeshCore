// Companion identity = stock examples/companion_radio/MyMesh.cpp, compiled here
// under the name CompanionMesh. The example source is NOT modified.
// Driven by the MeshCore phone app over WiFi/TCP port 5000 (the stock
// SerialWifiInterface frame transport), same as a stock WIFI_SSID companion
// build. The TCP listener starts lazily once the composition has WiFi up
// (station connected, or the first-boot setup AP).

// keep this stock example headless even if the env defines the web panel
#ifdef WITH_WEB_PANEL
#undef WITH_WEB_PANEL
#endif
#ifdef WITH_MQTT_UPLINK
#undef WITH_MQTT_UPLINK
#endif

#define MyMesh CompanionMesh
#include "../companion_radio/MyMesh.cpp"
#undef MyMesh

#include "identity_module.h"
#include "diag_service.h"
#include "identity_backup.h"
#include "multi_web.h"
#include <target.h>
#include <WiFi.h>
#include <helpers/esp32/SerialWifiInterface.h>

#ifndef COMPANION_TCP_PORT
  #define COMPANION_TCP_PORT 5000   // what the MeshCore app expects for WiFi companions
#endif

#include "mux_serial.h"

// The main chat identity answers the same public "!" commands as the optional
// slots, so the diagnostics service works without enabling an extra identity.
// A recognised command is answered and NOT queued: the responder has dealt with
// it, and a paired phone should not buzz for every stranger's ping. Anything
// else falls through untouched, so normal messaging is unaffected.
class DiagCompanionMesh : public CompanionMesh {
public:
  DiagCompanionMesh(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc,
                    SimpleMeshTables& tables, DataStore& store)
    : CompanionMesh(radio, rng, rtc, tables, store, NULL) {}

  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp,
                     const char* text) override {
    char reply[DIAG_REPLY_MAX];
    if (diagEnabled() && diagBuildReply(text, pkt, getRTCClock(), reply, sizeof(reply))) {
      uint32_t ack, timeout;
      sendMessage(from, getRTCClock()->getCurrentTimeUnique(), 0, reply, ack, timeout);
      return;
    }
    CompanionMesh::onMessageRecv(from, pkt, sender_timestamp, text);
  }
};

static CompanionMesh*   g_comp = nullptr;
static StdRNG           comp_rng;
static SimpleMeshTables comp_tables;
static DataStore*       comp_store = nullptr;
static MuxSerialInterface comp_serial;
static bool             comp_tcp_started = false;
static SemaphoreHandle_t comp_web_mutex = nullptr;

static void comp_setup(MultiFS* fs, mesh::Radio* port) {
  comp_rng.begin(radio_driver.getRngSeed());
  comp_store = new DataStore(*fs, rtc_clock);
  comp_serial.init(40 * 1024);          // response buffer (PSRAM) — fits a full contact sync
  comp_web_mutex = xSemaphoreCreateMutex();
  g_comp = new DiagCompanionMesh(*port, comp_rng, rtc_clock, comp_tables, *comp_store);
  // The companion loads/creates its identity inside begin() (via DataStore),
  // so we can't intervene mid-flight: instead pre-heal the file from the NVS
  // mirror if it's missing, and mirror whatever it ends up using afterwards.
  {
    mesh::LocalIdentity pre;
    multiIdLoad("companion", fs, pre);   // restores the file when only the mirror survives
  }
  g_comp->begin(false);                 // loads/creates its own identity via DataStore
  multiIdImportSaveMirror("companion", g_comp->self_id);
  g_comp->startInterface(comp_serial);
  Serial.print("[companion] ID: ");
  mesh::Utils::printHex(Serial, g_comp->self_id.pub_key, PUB_KEY_SIZE); Serial.println();
}

static void comp_loop() {
  if (!comp_tcp_started) {
    // WiFi is owned by the composition and comes up after module setup(), so
    // defer binding the listener until the stack is actually usable (station
    // got an IP, or the first-boot setup AP is running).
    if (WiFi.status() == WL_CONNECTED || (WiFi.getMode() & WIFI_MODE_AP)) {
      comp_serial.tcp.begin(COMPANION_TCP_PORT);
      comp_tcp_started = true;
      Serial.printf("[companion] app interface listening on TCP port %d\n", COMPANION_TCP_PORT);
    }
  }
  if (g_comp) g_comp->loop();
}

static void comp_cmd(const char* c, char* r, size_t n) {
  if (!n) return;
  if (*c == 0 || strcmp(c, "status") == 0) {
    snprintf(r, n, "app interface tcp/%d: %s, client: %s", COMPANION_TCP_PORT,
             comp_tcp_started ? "listening" : "waiting for WiFi",
             comp_serial.isConnected() ? "connected" : "none");
  } else {
    snprintf(r, n, "companion is driven by the MeshCore app (WiFi -> this node's IP, port %d) or the /multi web page; console: status", COMPANION_TCP_PORT);
  }
}

// ---- web frame exchange (httpd task) — see multi_web.h ----
int compWebFrameExchange(const uint8_t* frame, size_t len,
                         uint8_t* out, size_t out_cap,
                         uint32_t total_ms, uint32_t idle_ms) {
  if (g_comp == nullptr || comp_web_mutex == nullptr) return -1;
  if (xSemaphoreTake(comp_web_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return -1;
  if (!comp_serial.webStart(frame, len)) {
    xSemaphoreGive(comp_web_mutex);
    return -1;
  }
  uint32_t start = millis();
  while (millis() - start < total_ms) {
    vTaskDelay(pdMS_TO_TICKS(25));
    if (comp_serial.webRespLen() > 0 && comp_serial.webMsSinceLastResp() >= idle_ms) break;
  }
  int n = comp_serial.webFinish(out, out_cap);
  xSemaphoreGive(comp_web_mutex);
  return n;
}

bool compTcpStarted() { return comp_tcp_started; }
bool compTcpClientConnected() { return comp_serial.isConnected(); }

uint32_t compArchiveSeq() { return comp_serial.archiveSeq(); }
int compArchiveCopy(uint32_t after, uint8_t* out, size_t cap) {
  return comp_serial.archiveCopy(after, out, cap);
}

static void comp_pk(uint8_t out[32]) { if (g_comp) memcpy(out, g_comp->self_id.pub_key, PUB_KEY_SIZE); }

static uint32_t comp_pool_full() { return g_comp ? g_comp->getNumRxPoolFull() : 0; }

IdentityModule companion_module = { "companion", comp_setup, comp_loop, comp_cmd, comp_pk, comp_pool_full };
