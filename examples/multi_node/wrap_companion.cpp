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
#include "identity_backup.h"
#include "multi_web.h"
#include <target.h>
#include <WiFi.h>
#include <helpers/esp32/SerialWifiInterface.h>

#ifndef COMPANION_TCP_PORT
  #define COMPANION_TCP_PORT 5000   // what the MeshCore app expects for WiFi companions
#endif

// Frame mux: lets the web panel inject app-protocol frames into the (single)
// companion serial interface alongside the phone app's TCP session.
//
// Routing rules (single-threaded mesh loop; responses to a command are written
// synchronously while that command is being handled):
//   * checkRecvFrame() prefers a pending web frame; consuming one marks the
//     web as the owner of subsequent responses, consuming a TCP frame marks
//     the app as owner.
//   * writeFrame(): async push codes (>= 0x80) always go to the app's TCP
//     socket; response codes (< 0x80) go to whichever side issued the command
//     being processed.
class MuxSerialInterface : public BaseSerialInterface {
public:
  SerialWifiInterface tcp;

  void init(size_t resp_cap) {
    _resp = (uint8_t*)ps_malloc(resp_cap);
    if (_resp == nullptr) { resp_cap = 4096; _resp = (uint8_t*)malloc(resp_cap); }
    _resp_cap = _resp ? resp_cap : 0;
    _arch = (ArchSlot*)ps_malloc(sizeof(ArchSlot) * ARCH_SLOTS);
    if (_arch != nullptr) memset(_arch, 0, sizeof(ArchSlot) * ARCH_SLOTS);
  }

  // BaseSerialInterface (called from the mesh loop task)
  void enable() override { tcp.enable(); }
  void disable() override { tcp.disable(); }
  bool isEnabled() const override { return tcp.isEnabled(); }
  // Report "connected" while the web UI is actively exchanging frames too:
  // stock code gates some async results (e.g. trace data) on isConnected(),
  // and the web needs those mirrored even with no phone attached.
  bool isConnected() const override {
    return tcp.isConnected() || (millis() - _last_web_ms < 60000);
  }
  bool isWriteBusy() const override { return tcp.isWriteBusy(); }

  size_t checkRecvFrame(uint8_t dest[]) override {
    if (_in_state == 1) {
      size_t n = _in_len;
      memcpy(dest, _in_buf, n);
      _route_web = true;
      _last_resp_ms = millis();
      _in_state = 2;
      return n;
    }
    size_t n = tcp.checkRecvFrame(dest);
    if (n > 0) _route_web = false;
    return n;
  }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    // Mirror every message frame passing through (whichever side synced it)
    // into the archive, so the web UI can show history without consuming
    // anything from the companion's offline queue. Trace results (0x89) are
    // mirrored too so the web traceroute can pick them up.
    if (len > 0 && (src[0] == 7 || src[0] == 8 || src[0] == 16 || src[0] == 17 || src[0] == 27 ||
                    src[0] == 0x89)) {
      archiveAdd(src, len);
    }
    if (len > 0 && src[0] >= 0x80) {   // async push -> phone app
      return tcp.writeFrame(src, len);
    }
    if (_route_web && _in_state == 2 && _resp != nullptr) {
      if (_resp_len + 2 + len <= _resp_cap) {
        _resp[_resp_len] = len & 0xFF;
        _resp[_resp_len + 1] = (len >> 8) & 0xFF;
        memcpy(_resp + _resp_len + 2, src, len);
        _resp_len = _resp_len + 2 + len;
        _last_resp_ms = millis();
      }
      return len;
    }
    return tcp.writeFrame(src, len);
  }

  // web side (called from the httpd task; caller holds the exchange mutex)
  bool webStart(const uint8_t* frame, size_t len) {
    if (_in_state != 0 || len == 0 || len > sizeof(_in_buf)) return false;
    memcpy(_in_buf, frame, len);
    _in_len = len;
    _resp_len = 0;
    _last_resp_ms = millis();
    _last_web_ms = millis();
    _in_state = 1;   // set last: publishes the frame to the mesh loop
    return true;
  }
  size_t webRespLen() const { return _resp_len; }
  uint32_t webMsSinceLastResp() const { return millis() - _last_resp_ms; }
  int webFinish(uint8_t* out, size_t out_cap) {
    size_t n = _resp_len;
    if (n > out_cap) n = out_cap;
    if (n > 0) memcpy(out, _resp, n);
    _route_web = false;
    _resp_len = 0;
    _in_state = 0;
    return (int)n;
  }

  // message archive (mirror of synced message frames; read-only for the web)
  uint32_t archiveSeq() const { return _arch_seq; }
  // copy entries with seq > after into out as [u32 seq][u16 len][frame]...
  int archiveCopy(uint32_t after, uint8_t* out, size_t cap) {
    if (_arch == nullptr) return 0;
    uint32_t newest = _arch_seq;
    uint32_t oldest = newest > ARCH_SLOTS ? newest - ARCH_SLOTS : 0;
    if (after < oldest) after = oldest;
    size_t o = 0;
    for (uint32_t s = after + 1; s <= newest; s++) {
      ArchSlot& a = _arch[(s - 1) % ARCH_SLOTS];
      if (a.seq != s) continue;               // overwritten mid-copy
      if (o + 6 + a.len > cap) break;
      memcpy(out + o, &s, 4);
      out[o + 4] = a.len & 0xFF; out[o + 5] = (a.len >> 8) & 0xFF;
      memcpy(out + o + 6, a.buf, a.len);
      o += 6 + a.len;
    }
    return (int)o;
  }

private:
  static const int ARCH_SLOTS = 128;
  struct ArchSlot { uint32_t seq; uint16_t len; uint8_t buf[MAX_FRAME_SIZE]; };
  ArchSlot* _arch = nullptr;
  volatile uint32_t _arch_seq = 0;

  void archiveAdd(const uint8_t* src, size_t len) {
    if (_arch == nullptr || len > MAX_FRAME_SIZE) return;
    ArchSlot& a = _arch[_arch_seq % ARCH_SLOTS];
    a.len = (uint16_t)len;
    memcpy(a.buf, src, len);
    a.seq = _arch_seq + 1;   // written last
    _arch_seq = _arch_seq + 1;
  }

  volatile uint8_t _in_state = 0;   // 0 idle, 1 frame pending, 2 consumed (collecting responses)
  uint8_t  _in_buf[MAX_FRAME_SIZE];
  volatile size_t _in_len = 0;
  uint8_t* _resp = nullptr;
  size_t   _resp_cap = 0;
  volatile size_t _resp_len = 0;
  volatile uint32_t _last_resp_ms = 0;
  volatile uint32_t _last_web_ms = 0;
  volatile bool _route_web = false;
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
  g_comp = new CompanionMesh(*port, comp_rng, rtc_clock, comp_tables, *comp_store, NULL);
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

IdentityModule companion_module = { "companion", comp_setup, comp_loop, comp_cmd, comp_pk };
