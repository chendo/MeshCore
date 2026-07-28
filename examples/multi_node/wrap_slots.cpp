// Optional identity slots: extra chat (companion) identities beyond the three
// fixed roles. Each enabled slot is a full, independent MeshCore node — its own
// keypair, contacts, storage view (/fs/chatN) and radio port — driven by the
// MeshCore phone app on its own TCP port (COMPANION_TCP_PORT + slot).
//
// Slots are enabled/disabled from the web panel; the setting is stored in NVS
// and applied at boot, because radio ports must all be registered before the
// shared-radio frame fan-out starts. A disabled slot costs nothing at runtime
// (no port, no loop time, no mesh RAM) and its identity/data stay on the
// filesystem, so re-enabling brings the same node back.

#ifdef WITH_WEB_PANEL
#undef WITH_WEB_PANEL
#endif
#ifdef WITH_MQTT_UPLINK
#undef WITH_MQTT_UPLINK
#endif

#define MyMesh SlotChatMesh
#include "../companion_radio/MyMesh.cpp"
#undef MyMesh

#include "identity_module.h"
#include "identity_backup.h"
#include "diag_service.h"
#include "multi_web.h"
#include "mux_serial.h"
#include <target.h>
#include <WiFi.h>
#include <Preferences.h>

#ifndef COMPANION_TCP_PORT
  #define COMPANION_TCP_PORT 5000
#endif

struct ChatSlot {
  SlotChatMesh*     mesh;
  StdRNG            rng;
  SimpleMeshTables  tables;
  DataStore*        store;
  MuxSerialInterface serial;
  SemaphoreHandle_t mutex;
  bool              tcp_started;
  char              name[8];    // "chat1" ...
  char              fsdir[12];  // "/fs/chat1"
};

// A chat identity with nobody's phone attached just accumulates messages it can
// never deliver. When the diagnostics service is on, a "!" command is answered
// on the spot and NOT queued; anything else falls through to normal behaviour,
// so a slot that is genuinely paired with an app is unaffected.
class DiagChatMesh : public SlotChatMesh {
public:
  DiagChatMesh(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc,
               SimpleMeshTables& tables, DataStore& store)
    : SlotChatMesh(radio, rng, rtc, tables, store, NULL) {}

  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp,
                     const char* text) override {
    char reply[DIAG_REPLY_MAX];
    if (diagEnabled() && diagBuildReply(text, pkt, getRTCClock(), reply, sizeof(reply))) {
      uint32_t ack, timeout;
      // reply as a normal DM: direct when we know a path back, flood otherwise
      sendMessage(from, getRTCClock()->getCurrentTimeUnique(), 0, reply, ack, timeout);
      return;                       // handled — don't queue it for an absent app
    }
    SlotChatMesh::onMessageRecv(from, pkt, sender_timestamp, text);
  }
};

static ChatSlot s_slots[MULTI_MAX_CHAT_SLOTS];
static IdentityModule s_modules[MULTI_MAX_CHAT_SLOTS];

// ---- slot enable flags (NVS, read at boot) ----
bool multiChatSlotEnabled(int i) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return false;
  char key[8]; snprintf(key, sizeof(key), "chat%d", i + 1);
  Preferences p; p.begin("multislots", true);
  bool en = p.getBool(key, false);
  p.end();
  return en;
}
void multiChatSlotSetEnabled(int i, bool en) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return;
  char key[8]; snprintf(key, sizeof(key), "chat%d", i + 1);
  Preferences p; p.begin("multislots", false);
  p.putBool(key, en);
  p.end();
}
int multiChatSlotPort(int i) { return COMPANION_TCP_PORT + 1 + i; }

// ---- per-slot module callbacks (index recovered from the module name) ----
static int slotIndexFor(const char* name) {
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) if (s_modules[i].name == name) return i;
  return -1;
}

template <int I>
static void slot_setup(MultiFS* fs, mesh::Radio* port) {
  ChatSlot& s = s_slots[I];
  s.rng.begin(radio_driver.getRngSeed());
  s.store = new DataStore(*fs, rtc_clock);
  s.serial.init(16 * 1024);
  s.mutex = xSemaphoreCreateMutex();
  s.mesh = new DiagChatMesh(*port, s.rng, rtc_clock, s.tables, *s.store);
  { mesh::LocalIdentity pre; multiIdLoad(s.name, fs, pre); }   // heal from mirror if needed
  s.mesh->begin(false);
  multiIdImportSaveMirror(s.name, s.mesh->self_id);
  s.mesh->startInterface(s.serial);
  Serial.printf("[%s] ID: ", s.name);
  mesh::Utils::printHex(Serial, s.mesh->self_id.pub_key, PUB_KEY_SIZE); Serial.println();
}

template <int I>
static void slot_loop() {
  ChatSlot& s = s_slots[I];
  if (!s.tcp_started && (WiFi.status() == WL_CONNECTED || (WiFi.getMode() & WIFI_MODE_AP))) {
    s.serial.tcp.begin(multiChatSlotPort(I));
    s.tcp_started = true;
    Serial.printf("[%s] app interface listening on TCP port %d\n", s.name, multiChatSlotPort(I));
  }
  if (s.mesh) s.mesh->loop();
}

template <int I>
static void slot_cmd(const char* c, char* r, size_t n) {
  ChatSlot& s = s_slots[I];
  if (!n) return;
  snprintf(r, n, "chat slot %d: tcp/%d %s, client: %s", I + 1, multiChatSlotPort(I),
           s.tcp_started ? "listening" : "waiting for WiFi",
           s.serial.isConnected() ? "connected" : "none");
}

template <int I>
static void slot_pk(uint8_t out[32]) {
  if (s_slots[I].mesh) memcpy(out, s_slots[I].mesh->self_id.pub_key, PUB_KEY_SIZE);
}

template <int I>
static uint32_t slot_pool_full() {
  return s_slots[I].mesh ? s_slots[I].mesh->getNumRxPoolFull() : 0;
}

// Build the module table once. Templates give each slot its own callbacks
// without duplicating the mesh implementation (one SlotChatMesh class serves
// them all — only the instances differ).
IdentityModule* multiChatSlotModule(int i) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return nullptr;
  return &s_modules[i];
}
const char* multiChatSlotFsDir(int i) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return "";
  return s_slots[i].fsdir;
}

void multiChatSlotsInit() {
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    snprintf(s_slots[i].name, sizeof(s_slots[i].name), "chat%d", i + 1);
    snprintf(s_slots[i].fsdir, sizeof(s_slots[i].fsdir), "/fs/chat%d", i + 1);
    s_slots[i].mesh = nullptr; s_slots[i].store = nullptr;
    s_slots[i].tcp_started = false; s_slots[i].mutex = nullptr;
    s_modules[i].name = s_slots[i].name;
  }
  #define WIRE(N) \
    s_modules[N].setup = &slot_setup<N>; s_modules[N].loop = &slot_loop<N>; \
    s_modules[N].run_command = &slot_cmd<N>; s_modules[N].get_pubkey = &slot_pk<N>; \
    s_modules[N].rx_pool_full = &slot_pool_full<N>;
  WIRE(0) WIRE(1) WIRE(2) WIRE(3) WIRE(4)
  #undef WIRE
}

// ---- web access to a slot's app-protocol interface ----
int multiChatSlotFrameExchange(int i, const uint8_t* frame, size_t len,
                               uint8_t* out, size_t out_cap,
                               uint32_t total_ms, uint32_t idle_ms) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS) return -1;
  ChatSlot& s = s_slots[i];
  if (s.mesh == nullptr || s.mutex == nullptr) return -1;
  if (xSemaphoreTake(s.mutex, pdMS_TO_TICKS(250)) != pdTRUE) return -1;
  if (!s.serial.webStart(frame, len)) { xSemaphoreGive(s.mutex); return -1; }
  uint32_t start = millis();
  while (millis() - start < total_ms) {
    vTaskDelay(pdMS_TO_TICKS(25));
    if (s.serial.webRespLen() > 0 && s.serial.webMsSinceLastResp() >= idle_ms) break;
  }
  int n = s.serial.webFinish(out, out_cap);
  xSemaphoreGive(s.mutex);
  return n;
}
uint32_t multiChatSlotArchiveSeq(int i) {
  return (i >= 0 && i < MULTI_MAX_CHAT_SLOTS && s_slots[i].mesh) ? s_slots[i].serial.archiveSeq() : 0;
}
int multiChatSlotArchiveCopy(int i, uint32_t after, uint8_t* out, size_t cap) {
  if (i < 0 || i >= MULTI_MAX_CHAT_SLOTS || s_slots[i].mesh == nullptr) return 0;
  return s_slots[i].serial.archiveCopy(after, out, cap);
}
bool multiChatSlotRunning(int i) {
  return i >= 0 && i < MULTI_MAX_CHAT_SLOTS && s_slots[i].mesh != nullptr;
}
bool multiChatSlotClient(int i) {
  return i >= 0 && i < MULTI_MAX_CHAT_SLOTS && s_slots[i].mesh && s_slots[i].serial.isConnected();
}
