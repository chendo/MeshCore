// The unified web panel ("/") + JSON/binary endpoints, registered on the stock
// web server via WebPanelServer::setExtRoutesRegistrar() with index takeover.
// Reuses the panel's /login session token for auth. Stock SPA remains at /app.
//
// Endpoints:
//   GET  /                          the unified panel (web_multi_page.h)
//   GET  /multi                     redirect to / (legacy bookmark)
//   GET  /api/multi/debug           JSON: identities, wifi, stats, neighbors, companion state
//   GET  /api/multi/packets?after=N JSON: radio packet trace from SharedRadioCore
//   GET  /api/multi/stats?series=X  JSON: sampled history (battery/heap/wifi_rssi/noise/packets)
//   POST /api/multi/comp/frame      binary app-protocol frame in -> [u16 len][frame]... out
//   GET  /api/multi/comp/archive    read-only mirror of synced message frames

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <nvs.h>                           // NVS health (identity mirror lives here)
#include <target.h>                        // board (battery millivolts), rtc_clock
#include <helpers/SharedRadio.h>
#include <helpers/BaseSerialInterface.h>   // MAX_FRAME_SIZE
#include <helpers/web/WebPanelServer.h>
#include "multi_web.h"

// ---------- stats history sampler ----------
// One sample per minute into a PSRAM ring; 1440 samples = 24h of history.

struct StatSample {
  uint32_t t_s;        // uptime seconds at sample time
  uint16_t batt_mv;
  int16_t  wifi_rssi;
  int16_t  noise;      // radio noise floor dBm
  uint16_t heap_kb;
  uint32_t rx_total, tx_total;
  uint8_t  load_pct;   // main-task duty cycle
};
static const int STAT_SLOTS = 1440;
static StatSample* s_stats = nullptr;
static volatile uint32_t s_stat_seq = 0;
static uint32_t s_next_sample_ms = 0;

// ---- neighbours snapshot ----
// The repeater's neighbour table is RAM-only (its stock snapshot path needs an
// SD card). The composition remembers heard nodes across reboots/OTAs by
// periodically writing "prefix8,last_heard_epoch,snr4\n" lines to the shared
// FS, keyed by pubkey prefix, merged with live data by the web UI.

static uint32_t s_next_nbr_save_ms = 120000;   // let RTC/WiFi settle first
static uint32_t s_nbr_writes = 0;              // flash writes performed (diagnostics)

// Flash-wear-aware: the file is only REWRITTEN when the neighbour set changes
// (new node heard) or a node's saved last-heard has drifted by more than
// REFRESH_S. Live nodes constantly bump their timestamps, so writing every
// scan would rewrite the file forever for zero information gain; membership +
// coarse ages is the useful, stable content. Reads don't wear flash.
static void saveNeighboursSnapshot() {
  static const uint32_t REFRESH_S = 6 * 3600;
  static const int MAX_SAVED = 48;
  struct Entry { char pfx[9]; uint32_t epoch; int snr4; };

  char reply[1024];
  reply[0] = 0;
  multiRunConsole("neighbors", reply, sizeof(reply));
  if (reply[0] == 0 || strncmp(reply, "-none-", 6) == 0 || strncmp(reply, "Err", 3) == 0) return;

  Entry saved[MAX_SAVED];
  int n_saved = 0;
  { File f = multiSysFS()->open("/neighbours.csv", "r");
    if (f) {
      while (f.available() && n_saved < MAX_SAVED) {
        String l = f.readStringUntil('\n');
        int c1 = l.indexOf(','), c2 = l.indexOf(',', c1 + 1);
        if (c1 != 8 || c2 < 0) continue;
        Entry& e = saved[n_saved];
        l.substring(0, 8).toCharArray(e.pfx, sizeof(e.pfx));
        e.epoch = strtoul(l.substring(c1 + 1, c2).c_str(), nullptr, 10);
        e.snr4 = atoi(l.substring(c2 + 1).c_str());
        n_saved++;
      }
      f.close();
    } }

  uint32_t now_epoch = rtc_clock.getCurrentTime();
  bool dirty = false;
  { char* save = nullptr;
    for (char* line = strtok_r(reply, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
      char* c1 = strchr(line, ':'); if (!c1) continue;
      char* c2 = strchr(c1 + 1, ':'); if (!c2) continue;
      *c1 = 0; *c2 = 0;
      if (strlen(line) != 8) continue;
      uint32_t heard = now_epoch - (uint32_t)atol(c1 + 1);
      int snr4 = atoi(c2 + 1);
      int i = 0;
      while (i < n_saved && strcasecmp(saved[i].pfx, line) != 0) i++;
      if (i == n_saved) {                       // new neighbour -> must persist
        if (n_saved < MAX_SAVED) {
          strncpy(saved[n_saved].pfx, line, sizeof(saved[0].pfx));
          saved[n_saved].epoch = heard;
          saved[n_saved].snr4 = snr4;
          n_saved++;
          dirty = true;
        }
      } else if (heard > saved[i].epoch + REFRESH_S) {   // coarse freshness only
        saved[i].epoch = heard;
        saved[i].snr4 = snr4;
        dirty = true;
      }
    } }
  if (!dirty) return;

  File f = multiSysFS()->open("/neighbours.csv", "w", true);
  if (f) {
    for (int i = 0; i < n_saved; i++) {
      f.printf("%s,%lu,%d\n", saved[i].pfx, (unsigned long)saved[i].epoch, saved[i].snr4);
    }
    f.close();
    s_nbr_writes++;
  }
}

void multiWebTick() {
  uint32_t now = millis();
  if (now >= s_next_nbr_save_ms) {
    s_next_nbr_save_ms = now + 5 * 60000;
    saveNeighboursSnapshot();
  }
  if (now < s_next_sample_ms) return;
  s_next_sample_ms = now + 60000;
  if (s_stats == nullptr) {
    s_stats = (StatSample*)heap_caps_malloc(sizeof(StatSample) * STAT_SLOTS,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_stats == nullptr) return;
  }
  StatSample& s = s_stats[s_stat_seq % STAT_SLOTS];
  s.t_s = now / 1000;
  s.batt_mv = (uint16_t)board.getBattMilliVolts();
  s.wifi_rssi = (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : 0;
  SharedRadioCore* core = multiCore();
  s.noise = core ? (int16_t)core->real()->getNoiseFloor() : 0;
  s.heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
  s.rx_total = core ? core->rxTotal() : 0;
  s.tx_total = core ? core->txTotal() : 0;
  s.load_pct = multiLoadPct();
  s_stat_seq = s_stat_seq + 1;
}

static WebPanelServer* s_panel = nullptr;

// ---------- helpers ----------

static bool authOk(httpd_req_t* req) {
  return s_panel != nullptr && s_panel->extAuthorize(req);
}

static esp_err_t deny(httpd_req_t* req) {
  return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
}

// append src to dst as a JSON string body (escapes quotes/backslash/control)
static void jsonEscapeAppend(String& dst, const char* src) {
  for (const char* p = src; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { dst += '\\'; dst += c; }
    else if (c == '\n') dst += "\\n";
    else if (c == '\r') { }
    else if ((uint8_t)c < 0x20) dst += ' ';
    else dst += c;
  }
}

static void runConsoleInto(String& dst, const char* cmd) {
  char reply[1024];
  reply[0] = 0;
  multiRunConsole(cmd, reply, sizeof(reply));
  jsonEscapeAppend(dst, reply);
}

// ---------- /api/multi/debug ----------

static esp_err_t handleDebug(httpd_req_t* req) {
  if (!authOk(req)) return deny(req);

  String out;
  out.reserve(3072);
  out += "{\"identities\":\"";  runConsoleInto(out, "identities");
  out += "\",\"wifi\":\"";      runConsoleInto(out, "wifi");
  out += "\",\"neighbors\":\""; runConsoleInto(out, "neighbors");
  out += "\",\"stats\":{\"repeater\":{\"packets\":";
  { char r[1024]; r[0]=0; multiRunConsole("repeater stats-packets", r, sizeof(r)); out += (r[0]=='{') ? r : "null"; }
  out += ",\"core\":";
  { char r[1024]; r[0]=0; multiRunConsole("repeater stats-core", r, sizeof(r)); out += (r[0]=='{') ? r : "null"; }
  out += "},\"room\":{\"packets\":";
  { char r[1024]; r[0]=0; multiRunConsole("room stats-packets", r, sizeof(r)); out += (r[0]=='{') ? r : "null"; }
  out += ",\"core\":";
  { char r[1024]; r[0]=0; multiRunConsole("room stats-core", r, sizeof(r)); out += (r[0]=='{') ? r : "null"; }
  out += "}},\"companion\":{\"tcp\":";
  out += compTcpStarted() ? "true" : "false";
  out += ",\"client\":";
  out += compTcpClientConnected() ? "true" : "false";
  out += "},\"radio\":{\"noise\":";
  { SharedRadioCore* c = multiCore(); out += String(c ? c->real()->getNoiseFloor() : 0);
    out += ",\"rx\":"; out += String(c ? c->rxTotal() : 0);
    out += ",\"tx\":"; out += String(c ? c->txTotal() : 0);
    out += ",\"busy\":"; out += String(c ? c->txContention() : 0);
    out += ",\"stuck\":"; out += String(c ? c->txStuck() : 0);
    out += ",\"refused\":"; out += String(c ? c->txRefused() : 0);
    out += ",\"recoveries\":"; out += String(c ? c->radioRecoveries() : 0);
    out += ",\"rx_age_s\":"; out += String(c ? c->msSinceLastRx() / 1000 : 0); }
  out += "},\"nvs\":{";
  { nvs_stats_t st;
    if (nvs_get_stats(nullptr, &st) == ESP_OK) {
      out += "\"used\":"; out += String(st.used_entries);
      out += ",\"free\":"; out += String(st.free_entries);
      out += ",\"total\":"; out += String(st.total_entries);
    } else out += "\"used\":0,\"free\":0,\"total\":0";
  }
  out += "},\"saved_nbrs\":\"";
  { File f = multiSysFS()->open("/neighbours.csv", "r");
    if (f) {
      char buf[512]; int n = f.read((uint8_t*)buf, sizeof(buf) - 1); f.close();
      if (n > 0) { buf[n] = 0; jsonEscapeAppend(out, buf); }
    } }
  out += "\",\"clock\":\"";     runConsoleInto(out, "time");
  out += "\",\"load\":";
  out += String(multiLoadPct());
  out += ",\"lps\":";
  out += String(multiLoopsPerSec());
  out += ",\"nbr_writes\":";
  out += String(s_nbr_writes);
  out += ",\"epoch\":";
  out += String((unsigned long)rtc_clock.getCurrentTime());
  out += ",\"uptime_s\":";
  out += String(millis() / 1000);
  out += ",\"heap\":";
  out += String((unsigned)ESP.getFreeHeap());
  out += ",\"psram\":";
  out += String((unsigned)ESP.getFreePsram());
  out += "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
}

// ---------- /api/multi/packets ----------

static esp_err_t handlePackets(httpd_req_t* req) {
  if (!authOk(req)) return deny(req);
  SharedRadioCore* core = multiCore();
  if (core == nullptr) return httpd_resp_send_500(req);

  uint32_t after = 0;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
      after = strtoul(val, nullptr, 10);
    }
  }

  PktLogEntry* entries = (PktLogEntry*)malloc(sizeof(PktLogEntry) * SharedRadioCore::PKT_LOG_SIZE);
  if (entries == nullptr) return httpd_resp_send_500(req);
  int n = core->pktLogCopy(entries, SharedRadioCore::PKT_LOG_SIZE, after);

  String out;
  out.reserve(256 + n * (96 + PKT_RAW_CAP * 2));
  out += "{\"now\":";
  out += String(millis());
  out += ",\"pkts\":[";
  for (int i = 0; i < n; i++) {
    PktLogEntry& e = entries[i];
    if (i) out += ',';
    out += "{\"s\":"; out += String(e.seq);
    out += ",\"t\":"; out += String(e.t_ms);
    out += ",\"e\":"; out += String(e.flag);
    out += ",\"x\":"; out += String(e.aux);
    out += ",\"d\":";
    if (e.dir < 0) out += "\"rx\"";
    else { out += '"'; out += core->portName(e.dir); out += '"'; }
    out += ",\"h\":"; out += String(e.hdr);
    out += ",\"l\":"; out += String(e.len);
    out += ",\"snr\":"; out += String(e.snr4 / 4.0f, 1);
    out += ",\"rssi\":"; out += String(e.rssi);
    out += ",\"raw\":\"";
    static const char* hx = "0123456789abcdef";
    for (int b = 0; b < e.raw_len; b++) {
      out += hx[e.raw[b] >> 4]; out += hx[e.raw[b] & 15];
    }
    out += "\"}";
  }
  out += "]}";
  free(entries);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
}

// ---------- /api/multi/stats ----------
// ?series=battery|heap|wifi_rssi|noise|packets -> {"points":[[uptime_s,value],...]}
// packets returns the per-minute delta of rx+tx.

static esp_err_t handleStatsSeries(httpd_req_t* req) {
  if (!authOk(req)) return deny(req);

  char query[48], series[20] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "series", series, sizeof(series));
  }

  String out;
  out.reserve(16 * 1024);
  out += "{\"series\":\""; out += series; out += "\",\"points\":[";
  if (s_stats != nullptr) {
    uint32_t newest = s_stat_seq;
    uint32_t oldest = newest > STAT_SLOTS ? newest - STAT_SLOTS : 0;
    bool first = true;
    StatSample prev; bool have_prev = false;
    for (uint32_t i = oldest; i < newest; i++) {
      StatSample s = s_stats[i % STAT_SLOTS];
      long v; bool ok = true;
      if      (strcmp(series, "battery") == 0)   v = s.batt_mv;
      else if (strcmp(series, "heap") == 0)      v = s.heap_kb;
      else if (strcmp(series, "wifi_rssi") == 0) v = s.wifi_rssi;
      else if (strcmp(series, "noise") == 0)     v = s.noise;
      else if (strcmp(series, "load") == 0)      v = s.load_pct;
      else if (strcmp(series, "packets") == 0) {
        if (have_prev) v = (long)((s.rx_total + s.tx_total) - (prev.rx_total + prev.tx_total));
        else ok = false;
      } else ok = false;
      prev = s; have_prev = true;
      if (!ok) continue;
      if (!first) out += ',';
      first = false;
      out += "["; out += String(s.t_s); out += ","; out += String(v); out += "]";
    }
  }
  out += "]}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
}

// ---------- /api/multi/room/posts ----------

static esp_err_t handleRoomPosts(httpd_req_t* req) {
  if (!authOk(req)) return deny(req);
  char* buf = (char*)malloc(8192);
  if (buf == nullptr) return httpd_resp_send_500(req);
  roomGetPostsJson(buf, 8192);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
  free(buf);
  return rc;
}

// ---------- /api/multi/comp/frame ----------

static esp_err_t handleCompFrame(httpd_req_t* req) {
  if (!authOk(req)) return deny(req);

  // optional tuning: ?t=<total_ms>&i=<idle_ms>
  uint32_t total_ms = 2500, idle_ms = 300;
  char query[48];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[12];
    if (httpd_query_key_value(query, "t", val, sizeof(val)) == ESP_OK) total_ms = strtoul(val, nullptr, 10);
    if (httpd_query_key_value(query, "i", val, sizeof(val)) == ESP_OK) idle_ms = strtoul(val, nullptr, 10);
  }
  if (total_ms > 8000) total_ms = 8000;
  if (idle_ms > 2000) idle_ms = 2000;

  uint8_t frame[MAX_FRAME_SIZE];
  int len = req->content_len;
  if (len <= 0 || len > (int)sizeof(frame)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad frame size");
  }
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, (char*)frame + got, len - got);
    if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body read failed");
    got += r;
  }

  const size_t out_cap = 40 * 1024;
  uint8_t* out = (uint8_t*)heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (out == nullptr) out = (uint8_t*)malloc(8192);
  if (out == nullptr) return httpd_resp_send_500(req);

  int n = compWebFrameExchange(frame, len, out, out_cap, total_ms, idle_ms);
  if (n < 0) {
    free(out);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Companion busy");
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, (const char*)out, n);
  free(out);
  return rc;
}

// ---------- /api/multi/comp/archive ----------
// Read-only mirror of message frames (fed by whoever syncs — phone or web).
// Response: [u32 latest_seq LE] then per entry [u32 seq][u16 len][frame]...

static esp_err_t handleCompArchive(httpd_req_t* req) {
  if (!authOk(req)) return deny(req);

  uint32_t after = 0;
  char query[48];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
      after = strtoul(val, nullptr, 10);
    }
  }

  const size_t cap = 32 * 1024;
  uint8_t* out = (uint8_t*)heap_caps_malloc(cap + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (out == nullptr) out = (uint8_t*)malloc(8192 + 4);
  if (out == nullptr) return httpd_resp_send_500(req);

  uint32_t latest = compArchiveSeq();
  memcpy(out, &latest, 4);
  int n = compArchiveCopy(after, out + 4, cap);

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, (const char*)out, 4 + n);
  free(out);
  return rc;
}

// ---------- /multi page ----------

#include "web_multi_page.h"

static esp_err_t handlePage(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, MULTI_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleLegacyMultiRedirect(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, nullptr, 0);
}

// ---------- registration ----------

void multiWebRegisterRoutes(httpd_handle_t server, WebPanelServer* panel) {
  s_panel = panel;
  static const httpd_uri_t root_uri  = {.uri = "/", .method = HTTP_GET, .handler = &handlePage, .user_ctx = nullptr};
  static const httpd_uri_t old_uri   = {.uri = "/multi", .method = HTTP_GET, .handler = &handleLegacyMultiRedirect, .user_ctx = nullptr};
  static const httpd_uri_t debug_uri = {.uri = "/api/multi/debug", .method = HTTP_GET, .handler = &handleDebug, .user_ctx = nullptr};
  static const httpd_uri_t pkts_uri  = {.uri = "/api/multi/packets", .method = HTTP_GET, .handler = &handlePackets, .user_ctx = nullptr};
  static const httpd_uri_t stats_uri = {.uri = "/api/multi/stats", .method = HTTP_GET, .handler = &handleStatsSeries, .user_ctx = nullptr};
  static const httpd_uri_t comp_uri  = {.uri = "/api/multi/comp/frame", .method = HTTP_POST, .handler = &handleCompFrame, .user_ctx = nullptr};
  static const httpd_uri_t arch_uri  = {.uri = "/api/multi/comp/archive", .method = HTTP_GET, .handler = &handleCompArchive, .user_ctx = nullptr};
  static const httpd_uri_t posts_uri = {.uri = "/api/multi/room/posts", .method = HTTP_GET, .handler = &handleRoomPosts, .user_ctx = nullptr};
  httpd_register_uri_handler(server, &root_uri);
  httpd_register_uri_handler(server, &old_uri);
  httpd_register_uri_handler(server, &debug_uri);
  httpd_register_uri_handler(server, &pkts_uri);
  httpd_register_uri_handler(server, &stats_uri);
  httpd_register_uri_handler(server, &comp_uri);
  httpd_register_uri_handler(server, &arch_uri);
  httpd_register_uri_handler(server, &posts_uri);
}
