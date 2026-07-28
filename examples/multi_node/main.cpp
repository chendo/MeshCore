// Multi-identity composition root: one board + one radio, shared by several
// stock example meshes (each its own identity) through the SharedRadioCore
// arbiter. The composition (not any example) owns WiFi and a single HTTPS web
// panel, and dispatches management commands to whichever identity is targeted.

#include <Arduino.h>
#include <Mesh.h>
#include <Utils.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <target.h>
#include <helpers/SharedRadio.h>
#include <helpers/NetworkService.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/web/WebService.h>
#include "identity_module.h"
#include "multi_web.h"
#include "identity_backup.h"

#ifndef SETUP_AP_SSID
#define SETUP_AP_SSID "MeshCore-Multi-Setup"
#endif
#ifndef SETUP_AP_PWD
#define SETUP_AP_PWD  "meshcore"   // >= 8 chars
#endif

#ifndef ADMIN_PASSWORD
#define ADMIN_PASSWORD "password"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PWD
#define WIFI_PWD ""
#endif

// identity modules (defined in the wrap_*.cpp translation units)
extern IdentityModule repeater_module;
extern IdentityModule room_module;
extern IdentityModule companion_module;

static IdentityModule* g_modules[3 + MULTI_MAX_CHAT_SLOTS] = { &repeater_module, &room_module, &companion_module };
static int NUM_MODULES = 3;   // optional chat slots appended at boot (see setup)

// ONE shared SPIFFS partition, exposed to each identity as a subdirectory view.
// SPIFFS has a flat namespace, so a view just prefixes every path ("/identity"
// -> "/fs/rep/identity" at the VFS layer). Keeps the stock hardcoded paths
// isolated per identity while using a single mounted partition (needed for the
// dual-OTA-slot layout, and stays under ESP-IDF's 3-SPIFFS-mount limit).
#include <vfs_api.h>
class SubdirFS : public fs::FS {
public:
  SubdirFS() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}   // VFSImpl is global-namespace in vfs_api.h
  void begin(const char* mountpoint) { _impl->mountpoint(mountpoint); }
};
static fs::SPIFFSFS   fs_shared;
static SubdirFS       fs_rep, fs_room, fs_comp, fs_sys;
static SubdirFS       fs_chat[MULTI_MAX_CHAT_SLOTS];
static SharedRadioCore* g_core = nullptr;
static RadioPort        port_rep, port_room, port_comp;
static RadioPort        port_chat[MULTI_MAX_CHAT_SLOTS];
static int              g_slot_port_idx[MULTI_MAX_CHAT_SLOTS];
static bool             g_slot_started[MULTI_MAX_CHAT_SLOTS];   // setup() has run this boot
// requests from the web/serial task, applied in loop(): 0 none, 1 start, 2 stop
static volatile uint8_t g_slot_request[MULTI_MAX_CHAT_SLOTS];

static NetworkService   network;
static WebService       web;

static bool       g_setup_mode = false;   // SoftAP config portal active
static WebServer  g_setup_srv(80);

// Composition-owned WiFi credential store (dedicated NVS namespace), so the
// repeater's inert stock NetworkService can never clobber our credentials.
static Preferences g_wifi_nvs;
static void wifiStoreSet(const char* key, const char* val) {
  g_wifi_nvs.begin("multiwifi", false);
  g_wifi_nvs.putString(key, val ? val : "");
  g_wifi_nvs.end();
}
static String wifiStoreGet(const char* key) {
  g_wifi_nvs.begin("multiwifi", true);
  String v = g_wifi_nvs.getString(key, "");
  g_wifi_nvs.end();
  return v;
}
static void wifiStoreClear() {
  g_wifi_nvs.begin("multiwifi", false);
  g_wifi_nvs.remove("ssid");
  g_wifi_nvs.remove("pwd");
  g_wifi_nvs.end();
}

// web/serial admin password, persisted in the same composition NVS store.
static char g_admin_pwd[48] = ADMIN_PASSWORD;
static void loadAdminPwd() {
  g_wifi_nvs.begin("multiwifi", true);
  String p = g_wifi_nvs.getString("adminpwd", "");
  g_wifi_nvs.end();
  if (p.length() > 0) { strncpy(g_admin_pwd, p.c_str(), sizeof(g_admin_pwd) - 1); g_admin_pwd[sizeof(g_admin_pwd)-1] = 0; }
}
static void setAdminPwd(const char* p) {
  strncpy(g_admin_pwd, p ? p : "", sizeof(g_admin_pwd) - 1);
  g_admin_pwd[sizeof(g_admin_pwd) - 1] = 0;
  g_wifi_nvs.begin("multiwifi", false);
  g_wifi_nvs.putString("adminpwd", g_admin_pwd);
  g_wifi_nvs.end();
}

// ---- shared-radio ownership -------------------------------------------------
// All three identities compile their own stock CommonCLI "radio" get/set and
// each stores freq/bw/sf/cr in its OWN per-identity prefs, applying it to the
// (single, shared) radio_driver from its own begin(). Since setup() runs
// repeater -> room -> companion, companion's begin() runs last and silently
// wins, overwriting whatever the repeater/room had -- and the companion is a
// CLI-less BaseChatMesh, so its default can never be changed to match. Worse,
// the stock CommonCLI "set radio ..." only saves prefs ("reboot to apply"),
// so a change made via the panel appears to work, then un-sticks on reboot.
//
// Fix: the composition (which owns the physical radio) is the single source
// of truth. It stores freq/bw/sf/cr in its own NVS namespace, re-applies it
// live to radio_driver directly (bypassing per-identity prefs entirely) right
// after all three identities finish their own begin(), and intercepts
// "radio"/"get radio"/"set radio ..." in the command dispatcher below so the
// stock web panel's existing "Radio Settings" preset picker (dropdown of
// named community presets, fetched live from api.meshcore.nz) targets the
// shared radio instead of leaking through to the repeater's own prefs.
struct RadioParams { float freq, bw; uint8_t sf, cr; };
// matches the compiled-in default in all three stock examples (LORA_FREQ/BW/SF/CR)
static const RadioParams RADIO_DEFAULT = { 915.0f, 250.0f, 10, 5 };
static RadioParams g_radio = RADIO_DEFAULT;
static Preferences g_radio_nvs;

static bool radioParamsValid(float freq, float bw, uint8_t sf, uint8_t cr) {
  return freq >= 150.0f && freq <= 2500.0f && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 && bw >= 7.0f && bw <= 500.0f;
}

// accepts either the stock web panel's comma-separated "f,bw,sf,cr" or a
// space-separated "f bw sf cr" for serial/console convenience.
static bool parseRadioArgs(const char* args, RadioParams& out) {
  char tmp[64];
  strncpy(tmp, args, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
  const char* parts[4];
  int num = mesh::Utils::parseTextParts(tmp, parts, 4, ',');
  if (num < 4) {
    strncpy(tmp, args, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;   // parseTextParts mutates tmp
    num = mesh::Utils::parseTextParts(tmp, parts, 4, ' ');
  }
  if (num < 4) return false;
  out.freq = strtof(parts[0], nullptr);
  out.bw   = strtof(parts[1], nullptr);
  out.sf   = (uint8_t)atoi(parts[2]);
  out.cr   = (uint8_t)atoi(parts[3]);
  return radioParamsValid(out.freq, out.bw, out.sf, out.cr);
}

static void radioStoreSet(const RadioParams& p) {
  g_radio_nvs.begin("multiradio", false);
  g_radio_nvs.putFloat("freq", p.freq);
  g_radio_nvs.putFloat("bw", p.bw);
  g_radio_nvs.putUChar("sf", p.sf);
  g_radio_nvs.putUChar("cr", p.cr);
  g_radio_nvs.end();
}
static RadioParams radioStoreGet() {
  RadioParams p = RADIO_DEFAULT;
  g_radio_nvs.begin("multiradio", true);
  p.freq = g_radio_nvs.getFloat("freq", RADIO_DEFAULT.freq);
  p.bw   = g_radio_nvs.getFloat("bw", RADIO_DEFAULT.bw);
  p.sf   = (uint8_t)g_radio_nvs.getUChar("sf", RADIO_DEFAULT.sf);
  p.cr   = (uint8_t)g_radio_nvs.getUChar("cr", RADIO_DEFAULT.cr);
  g_radio_nvs.end();
  return radioParamsValid(p.freq, p.bw, p.sf, p.cr) ? p : RADIO_DEFAULT;
}
// applies live to the shared radio_driver -- no reboot needed, unlike the
// per-identity stock "set radio" which only takes effect after a restart.
static void applyRadioParams(const RadioParams& p) {
  radio_driver.setParams(p.freq, p.bw, p.sf, p.cr);
  g_radio = p;
}

// per-persona TX power via the real driver
class RealTxPower : public TxPowerControl {
public:
  void applyTxPower(int8_t dbm) override { radio_driver.setTxPower(dbm); }
};
static RealTxPower g_txpwr;

// ---- scheduled GPS time sync ------------------------------------------------
// The GPS receiver already disciplines the RTC whenever it is powered and has a
// fix, but it draws 20-40mA continuously — far too much to leave on for a
// fixed node. So run it on a duty cycle instead: wake it periodically, wait for
// a fix, take the time, and power it down again. A cold fix costs ~1-2 minutes
// of GPS current every SYNC_HOURS, which is negligible, and the node's clock
// stays accurate enough to be useful to its peers (every advert it sends
// carries this timestamp).
static const uint32_t GPS_FIX_TIMEOUT_MS = 150000;   // one hunting window
// While the pack is above GPS_MIN_BATT_MV the receiver STAYS ON and keeps
// hunting across windows instead of giving up for hours. A cold fix indoors,
// or under cover, routinely takes far longer than a single window — abandoning
// after one is why this node never once managed to discipline its clock. The
// battery gate still applies on every window, so a discharging pack ends the
// hunt rather than being drained by it.
static const uint16_t GPS_MIN_BATT_MV = 4000;        // don't spend charge on a sync when low
static const uint32_t GPS_BATT_RETRY_MS = 1800000;   // re-check battery in 30 min
static uint32_t g_gps_sync_hours = 4;                // 0 = never
static uint32_t g_gps_next_ms = 60000;               // first attempt a minute after boot
static uint32_t g_gps_deadline_ms = 0;               // non-zero while GPS is powered for a sync
static uint32_t g_gps_last_sync_epoch = 0;
static uint32_t g_gps_search_start_ms = 0;           // when the current hunt began
static uint32_t g_gps_skips_low_batt = 0;
static const char* g_gps_last_result = "not attempted yet";

// ---- clock drift history ----------------------------------------------------
// Every GPS sync measures how far the RTC had wandered since the previous one.
// Kept in RTC slow memory: it survives OTA/software reboots (NOT a power cycle)
// and, unlike NVS, costs no flash writes — this is diagnostics, not state
// worth wearing flash for.
struct ClockDriftSample {
  uint32_t epoch;      // GPS time at the sync
  int32_t  offset_s;   // gps - rtc  (positive => the RTC was running slow)
  uint32_t elapsed_s;  // since the previous sync (0 if this was the first)
};
#define DRIFT_MAGIC 0x44524654u   // 'DRFT'
#define DRIFT_SLOTS 24
RTC_NOINIT_ATTR static uint32_t g_drift_magic;
RTC_NOINIT_ATTR static ClockDriftSample g_drift[DRIFT_SLOTS];
RTC_NOINIT_ATTR static uint32_t g_drift_count;    // total ever recorded

static void driftInit() {
  if (g_drift_magic != DRIFT_MAGIC) {
    memset(g_drift, 0, sizeof(g_drift));
    g_drift_count = 0;
    g_drift_magic = DRIFT_MAGIC;
  }
}

static void driftRecord(uint32_t gps_epoch, int32_t offset_s, uint32_t elapsed_s) {
  driftInit();
  g_drift[g_drift_count % DRIFT_SLOTS] = { gps_epoch, offset_s, elapsed_s };
  g_drift_count++;
}

// parts-per-million from the most recent measured interval (0 if unknown)
static float driftPpm() {
  driftInit();
  if (g_drift_count == 0) return 0;
  const ClockDriftSample& s = g_drift[(g_drift_count - 1) % DRIFT_SLOTS];
  if (s.elapsed_s == 0) return 0;
  return (float)s.offset_s * 1000000.0f / (float)s.elapsed_s;
}

static void gpsPower(bool on) {
  sensors.setSettingValue("gps", on ? "1" : "0");
}

// "YYYY-MM-DD HH:MM:SSZ" from a unix epoch (UTC), without pulling in RTClib
static void formatEpochUtc(uint32_t epoch, char* out, size_t cap) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02dZ",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void gpsSyncStart() {
  gpsPower(true);
  g_gps_deadline_ms = millis() + GPS_FIX_TIMEOUT_MS;
  g_gps_search_start_ms = millis();
  g_gps_last_result = "acquiring fix...";
}

static void gpsSyncTick() {
  uint32_t now = millis();

  if (g_gps_deadline_ms == 0) {                       // idle: is a sync due?
    if (g_gps_sync_hours > 0 && now >= g_gps_next_ms) {
      uint16_t mv = board.getBattMilliVolts();
      if (mv > 0 && mv < GPS_MIN_BATT_MV) {
        // acquiring a fix costs a minute or two of GPS current; skip it while
        // the pack is low and look again shortly
        g_gps_skips_low_batt++;
        g_gps_last_result = "skipped: battery below threshold";
        g_gps_next_ms = now + GPS_BATT_RETRY_MS;
        return;
      }
      gpsSyncStart();
    }
    return;
  }

  LocationProvider* gps = sensors.getLocationProvider();
  if (gps != nullptr && gps->isValid()) {
    long ts = gps->getTimestamp();
    if (ts > 1700000000L) {                           // sane epoch (past 2023)
      // measure the RTC's error BEFORE correcting it — that difference, over
      // the interval since the last sync, is the crystal's real drift rate
      uint32_t before = rtc_clock.getCurrentTime();
      int32_t offset = (int32_t)((uint32_t)ts - before);
      uint32_t elapsed = g_gps_last_sync_epoch ? ((uint32_t)ts - g_gps_last_sync_epoch) : 0;
      driftRecord((uint32_t)ts, offset, elapsed);

      rtc_clock.setCurrentTime((uint32_t)ts);
      g_gps_last_sync_epoch = (uint32_t)ts;
      g_gps_last_result = "synced from GPS";
      if (elapsed) {
        Serial.printf("[gps] clock synced to %lu (RTC was %+ld s over %lu s = %.1f ppm)\n",
                      (unsigned long)ts, (long)offset, (unsigned long)elapsed,
                      (double)offset * 1000000.0 / (double)elapsed);
      } else {
        Serial.printf("[gps] clock synced to %lu (first sync, RTC was %+ld s out)\n",
                      (unsigned long)ts, (long)offset);
      }
      gpsPower(false);
      g_gps_deadline_ms = 0;
      g_gps_search_start_ms = 0;
      g_gps_next_ms = now + g_gps_sync_hours * 3600000UL;
      return;
    }
  }

  if (now >= g_gps_deadline_ms) {                     // window elapsed, still no fix
    uint16_t mv = board.getBattMilliVolts();
    if (mv == 0 || mv >= GPS_MIN_BATT_MV) {
      // Power to spare: stay on and keep hunting. Only the battery ends this.
      g_gps_deadline_ms = now + GPS_FIX_TIMEOUT_MS;
      g_gps_last_result = "searching (powered, battery ok)";
      return;
    }
    g_gps_skips_low_batt++;
    g_gps_last_result = "gave up: battery fell below threshold";
    Serial.printf("[gps] battery %umV below threshold after %lus of searching, powering down\n",
                  (unsigned)mv, (unsigned long)((now - g_gps_search_start_ms) / 1000));
    gpsPower(false);
    g_gps_deadline_ms = 0;
    g_gps_search_start_ms = 0;
    g_gps_next_ms = now + GPS_BATT_RETRY_MS;
  }
}


// ---- web command dispatcher: one console manages WiFi + all three identities
// Address an identity by prefixing its name: "room advert", "comp ...", etc.
// Bare commands go to the repeater (the public/primary node).
class MultiRunner : public WebPanelCommandRunner {
public:
  void runWebCommand(const char* command, char* reply, size_t reply_size) override {
    reply[0] = 0;
    if (!command) return;
    while (*command == ' ') command++;

    if (strcmp(command, "help") == 0 || strcmp(command, "commands") == 0 || strcmp(command, "?") == 0) {
      snprintf(reply, reply_size,
        "commands:\n"
        "  help                     this list\n"
        "  identities               list the three node pubkeys\n"
        "  wifi                     WiFi status (ssid/ip/rssi)\n"
        "  set wifi.ssid <name>     set WiFi network (persists)\n"
        "  set wifi.pwd <pass>      set WiFi password (persists)\n"
        "  wifi reset               clear WiFi, reboot to setup AP\n"
        "  set admin.pwd <pass>     set web/console admin password (persists)\n"
        "  radio                    show the shared radio (freq,bw,sf,cr) - governs all 3 identities\n"
        "  set radio <f,bw,sf,cr>   set the shared radio, applies live, persists, no reboot\n"
        "                           (same command the web panel's Radio Settings preset picker sends)\n"
        "  <cmd>                    run <cmd> on the REPEATER\n"
        "  repeater <cmd>           run <cmd> on the repeater\n"
        "  room <cmd>               run <cmd> on the room\n"
        "  companion <cmd>          (HTTP surface - WIP)\n"
        "  e.g.: advert | set lat -37.81 | set lon 144.96 | set name X | room advert");
      return;
    }

    // WiFi / network commands: persist to our own store AND push into NetworkService
    if (strncmp(command, "set wifi.ssid ", 14) == 0) {
      wifiStoreSet("ssid", command + 14);
      network.setWifiSSID(command + 14);
      snprintf(reply, reply_size, "OK - ssid saved");
      return;
    }
    if (strncmp(command, "set wifi.pwd ", 13) == 0) {
      wifiStoreSet("pwd", command + 13);
      network.setWifiPassword(command + 13);
      snprintf(reply, reply_size, "OK - pwd saved, reconnecting");
      return;
    }
    if (strcmp(command, "wifi reset") == 0) {
      wifiStoreClear();
      snprintf(reply, reply_size, "OK - WiFi cleared, rebooting to setup AP");
      delay(400);
      ESP.restart();
      return;
    }
    if (strncmp(command, "set admin.pwd ", 14) == 0) {
      const char* p = command + 14;
      if (strlen(p) < 4) { snprintf(reply, reply_size, "Err - min 4 chars"); return; }
      setAdminPwd(p);
      snprintf(reply, reply_size, "OK - admin password updated (re-unlock the panel)");
      return;
    }
    if (strcmp(command, "get wifi.status") == 0 || strcmp(command, "wifi") == 0) {
      snprintf(reply, reply_size, "ssid=%s connected=%s ip=%s rssi=%d powersave=%s",
               network.getWifiSSID()[0] ? network.getWifiSSID() : "-",
               WiFi.status() == WL_CONNECTED ? "yes" : "no",
               WiFi.localIP().toString().c_str(),
               WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
               network.getWifiPowerSave());
      return;
    }
    // ---- clock / GPS time discipline ----
    if (strcmp(command, "time") == 0 || strcmp(command, "clock") == 0) {
      uint32_t now_epoch = rtc_clock.getCurrentTime();
      LocationProvider* gps = sensors.getLocationProvider();
      char now_s[24], sync_s[24];
      formatEpochUtc(now_epoch, now_s, sizeof(now_s));
      if (g_gps_last_sync_epoch) formatEpochUtc(g_gps_last_sync_epoch, sync_s, sizeof(sync_s));
      else strncpy(sync_s, "never", sizeof(sync_s));
      float ppm = driftPpm();
      snprintf(reply, reply_size,
               "epoch=%lu utc=%s source=%s last_gps_sync=%s gps=%s sats=%ld "
               "next_sync_in=%lus every=%luh batt=%umV drift=%.1fppm(%.1fs/day) syncs=%lu skipped_low_batt=%lu",
               (unsigned long)now_epoch, now_s,
               g_gps_last_sync_epoch ? "gps" : "manual/unset", sync_s,
               g_gps_deadline_ms ? "on(acquiring)" : "off",
               gps ? gps->satellitesCount() : 0,
               (unsigned long)(g_gps_sync_hours == 0 ? 0 :
                 (g_gps_next_ms > millis() ? (g_gps_next_ms - millis()) / 1000 : 0)),
               (unsigned long)g_gps_sync_hours, (unsigned)board.getBattMilliVolts(),
               (double)ppm, (double)ppm * 86400.0 / 1000000.0,
               (unsigned long)g_drift_count, (unsigned long)g_gps_skips_low_batt);
      return;
    }
    if (strcmp(command, "clock drift") == 0) {
      driftInit();
      if (g_drift_count == 0) {
        snprintf(reply, reply_size, "no GPS syncs recorded yet (history clears on power loss)");
        return;
      }
      size_t o = 0;
      uint32_t shown = g_drift_count < DRIFT_SLOTS ? g_drift_count : DRIFT_SLOTS;
      uint32_t start = g_drift_count - shown;
      o += snprintf(reply + o, reply_size - o, "utc                  rtc_error  interval   rate\n");
      for (uint32_t i = start; i < g_drift_count && o + 64 < reply_size; i++) {
        const ClockDriftSample& s = g_drift[i % DRIFT_SLOTS];
        char ts[24];
        formatEpochUtc(s.epoch, ts, sizeof(ts));
        if (s.elapsed_s) {
          o += snprintf(reply + o, reply_size - o, "%s %+7lds %7luh  %+.1f ppm\n",
                        ts, (long)s.offset_s, (unsigned long)(s.elapsed_s / 3600),
                        (double)s.offset_s * 1000000.0 / (double)s.elapsed_s);
        } else {
          o += snprintf(reply + o, reply_size - o, "%s %+7lds       -   (first)\n",
                        ts, (long)s.offset_s);
        }
      }
      return;
    }
    if (strcmp(command, "gps sync") == 0) {
      if (g_gps_deadline_ms) { snprintf(reply, reply_size, "already acquiring a fix"); return; }
      gpsSyncStart();
      snprintf(reply, reply_size, "OK - GPS powered up, acquiring a fix (up to %lus)",
               (unsigned long)(GPS_FIX_TIMEOUT_MS / 1000));
      return;
    }
    if (strncmp(command, "set gps.sync ", 13) == 0) {
      g_gps_sync_hours = (uint32_t)atoi(command + 13);
      g_wifi_nvs.begin("multiwifi", false);
      g_wifi_nvs.putUInt("gpssync", g_gps_sync_hours);
      g_wifi_nvs.end();
      if (g_gps_sync_hours) g_gps_next_ms = millis() + 5000;
      snprintf(reply, reply_size, g_gps_sync_hours ? "OK - GPS clock sync every %lu h"
                                                   : "OK - GPS clock sync disabled",
               (unsigned long)g_gps_sync_hours);
      return;
    }
    if (strncmp(command, "set loopback ", 13) == 0) {
      bool on = strcmp(command + 13, "on") == 0 || strcmp(command + 13, "1") == 0;
      if (g_core) g_core->setLoopback(on);
      snprintf(reply, reply_size, "OK - on-board loopback %s (identities %s hear each other)",
               on ? "on" : "off", on ? "can" : "cannot");
      return;
    }
    if (strcmp(command, "get loopback") == 0) {
      snprintf(reply, reply_size, "> %s", (g_core && g_core->loopback()) ? "on" : "off");
      return;
    }
    if (strncmp(command, "set wifi.powersave ", 19) == 0) {
      if (network.setWifiPowerSave(command + 19)) {
        // authoritative copy in OUR store: the repeater's inert stock
        // NetworkService clobbers the shared eastmesh-net NVS namespace at
        // boot (same reason WiFi creds live in 'multiwifi')
        wifiStoreSet("ps", command + 19);
        snprintf(reply, reply_size, "OK - wifi powersave=%s (applies live, persists)", network.getWifiPowerSave());
      } else {
        snprintf(reply, reply_size, "Error - use: none | min | max");
      }
      return;
    }
    // shared radio: composition-authoritative, applies live (see note above)
    if (strcmp(command, "radio") == 0 || strcmp(command, "get radio") == 0) {
      char freq[16], bw[16];
      strcpy(freq, StrHelper::ftoa(g_radio.freq));
      strcpy(bw, StrHelper::ftoa3(g_radio.bw));
      snprintf(reply, reply_size, "> %s,%s,%d,%d", freq, bw, g_radio.sf, g_radio.cr);
      return;
    }
    if (strncmp(command, "set radio ", 10) == 0) {
      RadioParams p;
      if (!parseRadioArgs(command + 10, p)) {
        snprintf(reply, reply_size, "Error, invalid radio params");
        return;
      }
      applyRadioParams(p);
      radioStoreSet(p);
      snprintf(reply, reply_size, "OK - shared radio updated live for all 3 identities, no reboot needed");
      return;
    }
    // recovery: restore a known identity (192 hex = pub||prv) into both the
    // filesystem and the NVS mirror. Takes effect after a reboot.
    if (strncmp(command, "set identity.", 13) == 0) {
      const char* p = command + 13;
      const char* sp = strchr(p, ' ');
      if (sp == nullptr) { snprintf(reply, reply_size, "Error - usage: set identity.<role> <192 hex>"); return; }
      char role[16];
      size_t rl = (size_t)(sp - p);
      if (rl >= sizeof(role)) { snprintf(reply, reply_size, "Error - bad role"); return; }
      memcpy(role, p, rl); role[rl] = 0;
      fs::FS* target = strcmp(role, "repeater") == 0 ? (fs::FS*)&fs_rep
                     : strcmp(role, "room") == 0     ? (fs::FS*)&fs_room
                     : strcmp(role, "companion") == 0 ? (fs::FS*)&fs_comp : nullptr;
      if (target == nullptr) { snprintf(reply, reply_size, "Error - role must be repeater|room|companion"); return; }
      if (multiIdImport(role, target, sp + 1)) {
        snprintf(reply, reply_size, "OK - %s identity restored; reboot to apply", role);
      } else {
        snprintf(reply, reply_size, "Error - expected a valid 128-hex private key (or 192-hex pub||prv)");
      }
      return;
    }
    // optional chat identity slots: "set slot.chat2 on|off", "slots"
    if (strncmp(command, "set slot.chat", 13) == 0) {
      int idx = atoi(command + 13) - 1;
      const char* arg = strchr(command + 13, ' ');
      if (idx < 0 || idx >= MULTI_MAX_CHAT_SLOTS || arg == nullptr) {
        snprintf(reply, reply_size, "Error - usage: set slot.chat<1-%d> on|off", MULTI_MAX_CHAT_SLOTS);
        return;
      }
      bool on = strcmp(arg + 1, "on") == 0 || strcmp(arg + 1, "1") == 0;
      multiChatSlotSetEnabled(idx, on);
      g_slot_request[idx] = on ? 1 : 2;   // applied from loop(), no reboot needed
      snprintf(reply, reply_size, "OK - chat slot %d %s (app port %d)%s",
               idx + 1, on ? "starting" : "stopping", multiChatSlotPort(idx),
               on && !g_slot_started[idx] ? "" : "");
      return;
    }
    if (strcmp(command, "slots") == 0) {
      size_t o = 0;
      o += snprintf(reply + o, reply_size - o, "fixed: repeater, room, companion (app port %d)\n", 5000);
      for (int i = 0; i < MULTI_MAX_CHAT_SLOTS && o + 60 < reply_size; i++) {
        o += snprintf(reply + o, reply_size - o, "chat%d: %s%s (app port %d)\n", i + 1,
                      multiChatSlotEnabled(i) ? "enabled" : "disabled",
                      multiChatSlotRunning(i) ? " (running)" : "",
                      multiChatSlotPort(i));
      }
      return;
    }
    if (strcmp(command, "identities source") == 0) {   // how each key was obtained this boot
      multiIdSourceReport(reply, reply_size);
      return;
    }
    // Route the stock per-identity rekey through the composition so the NVS
    // mirror is updated too — otherwise the mirror would disagree on the next
    // boot and (correctly) revert the change as corruption.
    {
      const char* pk = nullptr; const char* role = nullptr; fs::FS* target = nullptr;
      if (strncmp(command, "set prv.key ", 12) == 0) { pk = command + 12; role = "repeater"; target = (fs::FS*)&fs_rep; }
      else if (strncmp(command, "repeater set prv.key ", 21) == 0) { pk = command + 21; role = "repeater"; target = (fs::FS*)&fs_rep; }
      else if (strncmp(command, "room set prv.key ", 17) == 0) { pk = command + 17; role = "room"; target = (fs::FS*)&fs_room; }
      if (pk != nullptr) {
        if (multiIdImport(role, target, pk)) {
          snprintf(reply, reply_size, "OK - %s identity set (filesystem + mirror); reboot to apply", role);
        } else {
          snprintf(reply, reply_size, "Error - bad private key");
        }
        return;
      }
    }
    if (strcmp(command, "identities full") == 0) {   // full 64-hex pubkeys (contact sharing/QR)
      size_t o = 0;
      for (int i = 0; i < NUM_MODULES && o + 80 < reply_size; i++) {
        uint8_t pk[32]; g_modules[i]->get_pubkey(pk);
        o += snprintf(reply + o, reply_size - o, "%s=", g_modules[i]->name);
        for (int b = 0; b < 32 && o + 3 < reply_size; b++) o += snprintf(reply + o, reply_size - o, "%02x", pk[b]);
        o += snprintf(reply + o, reply_size - o, "\n");
      }
      return;
    }
    if (strcmp(command, "identities") == 0) {
      size_t o = 0;
      for (int i = 0; i < NUM_MODULES; i++) {
        uint8_t pk[32]; g_modules[i]->get_pubkey(pk);
        o += snprintf(reply + o, reply_size - o, "%s=%02x%02x%02x%02x ",
                      g_modules[i]->name, pk[0], pk[1], pk[2], pk[3]);
      }
      return;
    }

    // identity routing: "<name> <cmd>"
    for (int i = 0; i < NUM_MODULES; i++) {
      size_t nlen = strlen(g_modules[i]->name);
      if (strncmp(command, g_modules[i]->name, nlen) == 0 && command[nlen] == ' ') {
        g_modules[i]->run_command(command + nlen + 1, reply, reply_size);
        return;
      }
    }
    // default: repeater
    repeater_module.run_command(command, reply, reply_size);
  }

  const char* getWebAdminPassword() const override { return g_admin_pwd; }
};
static MultiRunner g_runner;

// identity provenance log (storage for identity_backup.h — one copy only)
static char g_id_source[4][48];
static int  g_id_source_n = 0;
void multiIdNoteSource(const char* role, const char* how) {
  if (g_id_source_n < 4) snprintf(g_id_source[g_id_source_n++], 48, "%s: %s", role, how);
}
int multiIdSourceReport(char* out, size_t cap) {
  size_t o = 0;
  for (int i = 0; i < g_id_source_n && o < cap; i++) {
    o += snprintf(out + o, cap - o, "%s\n", g_id_source[i]);
  }
  if (o == 0 && cap) snprintf(out, cap, "(no identity events recorded)");
  return (int)o;
}

// GPS + clock state as JSON for the panel. The receiver is powered down between
// scheduled syncs, so "no fix" is the normal idle state rather than a fault —
// the panel needs enough context to tell those apart.
int multiGpsStatusJson(char* out, size_t cap) {
  LocationProvider* gps = sensors.getLocationProvider();
  uint32_t now = millis();
  bool powered = (g_gps_deadline_ms != 0);
  bool valid = (gps != nullptr && gps->isValid());
  long sats = gps ? gps->satellitesCount() : 0;
  uint32_t next_s = (g_gps_sync_hours == 0) ? 0
                    : (g_gps_next_ms > now ? (g_gps_next_ms - now) / 1000 : 0);
  driftInit();
  float ppm = driftPpm();

  int n = snprintf(out, cap,
    "{\"enabled\":%s,\"powered\":%s,\"lock\":%s,\"sats\":%ld,"
    "\"every_h\":%lu,\"next_s\":%lu,\"last_sync\":%lu,\"syncs\":%lu,"
    "\"skips_low_batt\":%lu,\"drift_ppm\":%.2f,\"searching_s\":%lu,\"state\":\"%s\","
    "\"clock_source\":\"%s\",\"epoch\":%lu",
    g_gps_sync_hours > 0 ? "true" : "false",
    powered ? "true" : "false",
    valid ? "true" : "false",
    sats,
    (unsigned long)g_gps_sync_hours, (unsigned long)next_s,
    (unsigned long)g_gps_last_sync_epoch, (unsigned long)g_drift_count,
    (unsigned long)g_gps_skips_low_batt, (double)ppm,
    (unsigned long)(g_gps_search_start_ms ? (now - g_gps_search_start_ms) / 1000 : 0),
    g_gps_last_result,
    g_gps_last_sync_epoch ? "gps" : "manual/unset",
    (unsigned long)rtc_clock.getCurrentTime());

  if (valid && n > 0 && (size_t)n < cap) {
    n += snprintf(out + n, cap - n, ",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%ld",
                  gps->getLatitude() / 1e6, gps->getLongitude() / 1e6, gps->getAltitude());
  }
  if (n > 0 && (size_t)n < cap) n += snprintf(out + n, cap - n, "}");
  return n;
}

// accessors for the unified web panel (web_multi.cpp)
SharedRadioCore* multiCore() { return g_core; }
// ---- console execution is confined to the loop task -------------------------
// The HTTPS server runs in its own FreeRTOS task, so a handler calling straight
// into runWebCommand() would execute mesh code CONCURRENTLY with the super-loop
// driving the same objects. That is a data race on live state — and not a
// theoretical one: 'advert' allocates from the shared packet pool and queues an
// outbound packet, which the dispatcher may be manipulating at that instant.
// Corrupting the pool loses packets, which is exactly what we must not do.
//
// So web/serial callers hand the command to the loop task and wait for it. The
// wait costs one loop iteration (~1-10ms) and makes every panel action safe by
// construction — the same approach already used for the companion frame mux
// and the identity slot toggles.
static TaskHandle_t     g_loop_task = nullptr;
static SemaphoreHandle_t g_console_mutex = nullptr;
static volatile bool    g_console_pending = false;
static volatile bool    g_console_done = false;
static char             g_console_cmd[512];
static char             g_console_reply[1024];

static void serviceConsoleRequest() {
  if (!g_console_pending) return;
  g_console_reply[0] = 0;
  g_runner.runWebCommand(g_console_cmd, g_console_reply, sizeof(g_console_reply));
  g_console_pending = false;
  g_console_done = true;
}

void multiRunConsole(const char* cmd, char* reply, size_t reply_size) {
  if (reply_size) reply[0] = 0;
  if (cmd == nullptr) return;

  // already on the loop task (serial console, boot-time config): run inline
  if (g_loop_task == nullptr || xTaskGetCurrentTaskHandle() == g_loop_task) {
    g_runner.runWebCommand(cmd, reply, reply_size);
    return;
  }

  if (g_console_mutex == nullptr ||
      xSemaphoreTake(g_console_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    snprintf(reply, reply_size, "busy - console unavailable");
    return;
  }
  strncpy(g_console_cmd, cmd, sizeof(g_console_cmd) - 1);
  g_console_cmd[sizeof(g_console_cmd) - 1] = 0;
  g_console_done = false;
  g_console_pending = true;                 // published last

  uint32_t start = millis();
  while (!g_console_done && (uint32_t)(millis() - start) < 4000) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (g_console_done) {
    strncpy(reply, g_console_reply, reply_size - 1);
    reply[reply_size - 1] = 0;
  } else {
    g_console_pending = false;              // give up rather than wedge the server
    snprintf(reply, reply_size, "timeout - node busy");
  }
  xSemaphoreGive(g_console_mutex);
}

// Same reasoning, generalised. Handlers that READ live mesh state (the room's
// post ring, for instance) race the loop task just as writers do — a ring being
// rotated underneath a reader yields a torn record, and following a pointer the
// loop task is rewriting can be worse than that. Those reads hand a function to
// the loop task instead.
//
// OWNERSHIP: if the loop task is too slow and we stop waiting, it will still
// run the call afterwards, so `arg` must outlive an abandoned request. Callers
// heap-allocate it and deliberately leak on timeout — a few kilobytes lost on a
// loop that has stalled for seconds beats writing into a freed stack frame.
static SemaphoreHandle_t   g_defer_mutex = nullptr;
static MultiLoopFn volatile g_defer_fn = nullptr;   // published last; null = idle
static void*               g_defer_arg = nullptr;

static void serviceDeferredCall() {
  MultiLoopFn fn = g_defer_fn;
  if (fn == nullptr) return;
  fn(g_defer_arg);
  g_defer_arg = nullptr;
  g_defer_fn = nullptr;         // cleared last: marks the slot free again
}

bool multiOnLoopTask() {
  return g_loop_task == nullptr || xTaskGetCurrentTaskHandle() == g_loop_task;
}

bool multiRunInLoop(MultiLoopFn fn, void* arg, uint32_t timeout_ms) {
  if (fn == nullptr) return false;
  if (multiOnLoopTask()) { fn(arg); return true; }
  if (g_defer_mutex == nullptr ||
      xSemaphoreTake(g_defer_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

  // A previous request that timed out is still queued; it must be allowed to
  // complete before the slot is reused, or the loop task would run it against
  // this caller's argument.
  uint32_t start = millis();
  while (g_defer_fn != nullptr) {
    if ((uint32_t)(millis() - start) >= timeout_ms) {
      xSemaphoreGive(g_defer_mutex);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  g_defer_arg = arg;
  g_defer_fn = fn;                          // published last

  bool done = false;
  start = millis();
  while ((uint32_t)(millis() - start) < timeout_ms) {
    if (g_defer_fn == nullptr) { done = true; break; }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  xSemaphoreGive(g_defer_mutex);
  return done;
}

// Packets this identity received and then discarded for want of a free Packet
// in its pool — a silent loss in the stock dispatcher. Keyed by arbiter port
// index so the panel can report it beside that port's other counters.
uint32_t multiPortPoolFull(int port_idx) {
  for (int i = 0; i < NUM_MODULES; i++) {
    int p = (i < 3) ? i : g_slot_port_idx[i - 3];
    if (p != port_idx) continue;
    return g_modules[i]->rx_pool_full ? g_modules[i]->rx_pool_full() : 0;
  }
  return 0;
}

fs::FS* multiSysFS() { return &fs_sys; }
void multiGetRadioParams(float* freq, float* bw, uint8_t* sf, uint8_t* cr) {
  if (freq) *freq = g_radio.freq;
  if (bw)   *bw = g_radio.bw;
  if (sf)   *sf = g_radio.sf;
  if (cr)   *cr = g_radio.cr;
}

void halt() { while (1); }

// Hot enable/disable of chat identity slots. Requests are queued by the web or
// serial task and applied HERE, from loop(), because starting a mesh allocates,
// touches the filesystem and registers with the arbiter — all of which must not
// race the mesh loop. Starting is complete (the slot goes on air immediately);
// stopping silences the identity — its port stops consuming frames and its
// loop() stops running — but the instance's memory is only reclaimed on reboot,
// since the stock mesh classes aren't built to be destroyed.
static void applySlotRequests() {
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    uint8_t req = g_slot_request[i];
    if (req == 0) continue;
    g_slot_request[i] = 0;
    IdentityModule* m = multiChatSlotModule(i);
    if (req == 1) {
      if (!g_slot_started[i]) {
        fs_chat[i].begin(multiChatSlotFsDir(i));
        m->setup(&fs_chat[i], &port_chat[i]);
        g_slot_started[i] = true;
        bool listed = false;
        for (int k = 0; k < NUM_MODULES; k++) if (g_modules[k] == m) listed = true;
        if (!listed && NUM_MODULES < (int)(sizeof(g_modules)/sizeof(g_modules[0]))) g_modules[NUM_MODULES++] = m;
        // the stock mesh applies ITS OWN stored radio params during begin();
        // the composition owns the shared radio, so put them back
        applyRadioParams(g_radio);
      }
      g_core->setPortActive(g_slot_port_idx[i], true);
      Serial.printf("[chat%d] started (no reboot)\n", i + 1);
    } else {
      g_core->setPortActive(g_slot_port_idx[i], false);
      for (int k = 0; k < NUM_MODULES; k++) {          // stop calling its loop()
        if (g_modules[k] == m) {
          for (int j = k; j < NUM_MODULES - 1; j++) g_modules[j] = g_modules[j + 1];
          NUM_MODULES--;
          break;
        }
      }
      Serial.printf("[chat%d] stopped (silenced; memory freed on next reboot)\n", i + 1);
    }
  }
}

// ---- SoftAP first-boot WiFi setup (no serial/BLE needed) --------------------
static const char SETUP_HTML[] =
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>MeshCore setup</title>"
  "<style>body{font-family:system-ui;margin:2rem;max-width:24rem}"
  "input{width:100%;padding:.6rem;margin:.4rem 0;box-sizing:border-box}"
  "button{padding:.6rem 1rem}</style>"
  "<h2>MeshCore WiFi setup</h2>"
  "<p>Enter your 2.4 GHz network. The node reboots and joins it.</p>"
  "<form method=POST action=/save>"
  "<label>Network (SSID)</label><input name=ssid required>"
  "<label>Password</label><input name=pwd type=password>"
  "<button type=submit>Save &amp; reboot</button></form>";

static void startSetupPortal() {
  g_setup_mode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PWD);
  Serial.printf("WiFi setup AP: '%s' pwd '%s' -> http://%s/\n",
                SETUP_AP_SSID, SETUP_AP_PWD, WiFi.softAPIP().toString().c_str());

  g_setup_srv.on("/", HTTP_GET, []() { g_setup_srv.send(200, "text/html", SETUP_HTML); });
  g_setup_srv.on("/save", HTTP_POST, []() {
    String ssid = g_setup_srv.arg("ssid");
    String pwd  = g_setup_srv.arg("pwd");
    if (ssid.length() == 0) { g_setup_srv.send(400, "text/plain", "ssid required"); return; }
    wifiStoreSet("ssid", ssid.c_str());
    wifiStoreSet("pwd", pwd.c_str());
    g_setup_srv.send(200, "text/html",
      "<meta http-equiv=refresh content='4;url=/'>Saved. Rebooting to join '" + ssid + "'...");
    delay(600);
    ESP.restart();
  });
  g_setup_srv.begin();
}

// run a CLI command against one identity, ignoring the reply (boot config)
static void cfg(IdentityModule* m, const char* cmd) {
  char reply[160];
  m->run_command(cmd, reply, sizeof(reply));
}

// advert policy: repeater is public and shares its location; room + companion
// suppress location. (Coordinates are set by the user via the panel:
// `repeater set lat <x>` / `repeater set lon <y>`.)
static void applyAdvertPolicy() {
  // Names are SEEDED only on an identity's first boot (no com_prefs yet):
  // running 'set name' unconditionally clobbered operator-set names on every
  // restart, since names persist via each identity's own prefs file.
  if (!fs_rep.exists("/com_prefs"))  cfg(&repeater_module, "set name " ADVERT_NAME " Repeater");
  if (!fs_room.exists("/com_prefs")) {
    // ROOMS ARE PRIVATE BY DEFAULT: a brand-new room neither announces itself
    // nor accepts the compile-time password everyone knows. It is discoverable
    // only via the panel's QR/meshcore:// share link, and joinable only with
    // the random password minted here. Operator changes always win afterwards
    // (this whole block is first-boot only).
    cfg(&room_module, "set name " ADVERT_NAME " Room");
    cfg(&room_module, "set advert.interval 0");
    cfg(&room_module, "set flood.advert.interval 0");

    static const char* ALPHABET = "abcdefghijkmnopqrstuvwxyz23456789";   // no look-alikes
    char pw[13];
    for (int i = 0; i < 12; i++) pw[i] = ALPHABET[esp_random() % 33];
    pw[12] = 0;
    char c[48];
    snprintf(c, sizeof(c), "set guest.password %s", pw);
    cfg(&room_module, c);
    Serial.printf("[room] private by default — adverts off, join password: %s\n", pw);
  }
  // location policy is a design constraint, applied every boot:
  cfg(&repeater_module, "gps advert prefs");     // repeater shares stored lat/lon
  cfg(&room_module,     "gps advert none");       // room never shares location
  // companion: BaseChatMesh, location off by default; name left at its default
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== MeshCore multi-identity ===");

  g_loop_task = xTaskGetCurrentTaskHandle();   // console commands must run here
  g_console_mutex = xSemaphoreCreateMutex();
  g_defer_mutex   = xSemaphoreCreateMutex();

  board.begin();
  if (!radio_init()) { Serial.println("Radio init failed!"); halt(); }
  sensors.begin();

  if (!fs_shared.begin(true, "/fs", 10, "fs")) Serial.println("WARN: shared FS mount failed");
  fs_rep.begin("/fs/rep");
  fs_room.begin("/fs/room");
  fs_comp.begin("/fs/comp");
  fs_sys.begin("/fs/sys");

  g_core = new SharedRadioCore(radio_driver);
  g_core->setTxPowerControl(&g_txpwr);
  // Put a wedged transceiver back together: full chip init, re-attach the
  // DIO1 ISR, then restore the shared radio parameters (radio_init() leaves
  // the compiled-in defaults, which are not what this node runs).
  g_core->setRadioReinit([]() {
    Serial.println("[radio] no packets for 15 min — re-initialising the transceiver");
    radio_init();
    radio_driver.begin();
    applyRadioParams(g_radio);
  });
  g_core->setRxErrorCounter([]() -> uint32_t { return radio_driver.getPacketsRecvErrors(); },
                            []() -> int16_t { return radio_driver.getLastRecvError(); },
                            []() -> const uint8_t* { return radio_driver.getLastRecvErrorPayload(); },
                            []() -> uint8_t { return radio_driver.getLastRecvErrorLen(); });
  g_core->setPortName(g_core->addPort(port_rep),  "repeater");
  g_core->setPortName(g_core->addPort(port_room), "room");
  g_core->setPortName(g_core->addPort(port_comp), "companion");

  // Optional chat identity slots. Every slot's port is registered up front —
  // inactive unless enabled — so port indices never move and a slot can be
  // switched on or off later without a reboot.
  multiChatSlotsInit();
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    IdentityModule* m = multiChatSlotModule(i);
    g_slot_port_idx[i] = g_core->addPort(port_chat[i], multiChatSlotEnabled(i));
    g_core->setPortName(g_slot_port_idx[i], m->name);
    if (multiChatSlotEnabled(i)) {
      fs_chat[i].begin(multiChatSlotFsDir(i));
      g_modules[NUM_MODULES++] = m;
    }
  }

  WebPanelServer::setExtRoutesRegistrar(&multiWebRegisterRoutes);   // unified panel + APIs
  WebPanelServer::setExtOwnsIndex(true);                            // panel served at "/" (stock SPA stays at /app)

  repeater_module.setup(&fs_rep, &port_rep);
  room_module.setup(&fs_room, &port_room);
  companion_module.setup(&fs_comp, &port_comp);
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    if (multiChatSlotEnabled(i)) { multiChatSlotModule(i)->setup(&fs_chat[i], &port_chat[i]); g_slot_started[i] = true; }
  }

  // tell the arbiter each identity's key so it can spot our own hash coming
  // back in another node's relayed path (see setPortIdentity)
  { uint8_t pk[32];
    repeater_module.get_pubkey(pk);  g_core->setPortIdentity(0, pk);
    room_module.get_pubkey(pk);      g_core->setPortIdentity(1, pk);
    companion_module.get_pubkey(pk); g_core->setPortIdentity(2, pk);
    for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
      if (!multiChatSlotEnabled(i)) continue;
      multiChatSlotModule(i)->get_pubkey(pk);
      g_core->setPortIdentity(g_slot_port_idx[i], pk);
    }
  }

  // authoritative last: overrides whatever each identity's own begin() just
  // applied to the shared radio_driver (see "shared-radio ownership" above)
  applyRadioParams(radioStoreGet());

  applyAdvertPolicy();

  loadAdminPwd();   // web/console admin password (persisted, defaults to ADMIN_PASSWORD)

  g_wifi_nvs.begin("multiwifi", true);
  g_gps_sync_hours = g_wifi_nvs.getUInt("gpssync", 4);   // GPS clock discipline interval
  g_wifi_nvs.end();
  driftInit();

  // composition owns WiFi + the single HTTPS web panel; credentials come from
  // our dedicated NVS store (authoritative), not NetworkService persistence.
  network.begin(&fs_sys, 0, "", "");
  String ss = wifiStoreGet("ssid");
  String pw = wifiStoreGet("pwd");

  if (ss.length() == 0) {
    // no WiFi configured -> first-boot SoftAP setup portal (no serial/BLE needed)
    startSetupPortal();
  } else {
    network.setWifiSSID(ss.c_str());
    network.setWifiPassword(pw.c_str());
    // powersave from OUR store (eastmesh-net NVS gets clobbered by the inert
    // stock NetworkService); default to modem sleep 'min' for power savings.
    {
      String ps = wifiStoreGet("ps");
      network.setWifiPowerSave(ps.length() ? ps.c_str() : "min");
    }
    web.setCommandRunner(&g_runner);
    web.setNetworkStateProvider(&network);
    web.begin(&fs_sys);
    board.setInhibitSleep(true);
  }

  board.onBootComplete();
  Serial.println("=== boot complete ===");
  if (g_setup_mode) {
    Serial.println("No WiFi set. Join AP '" SETUP_AP_SSID "' (pwd '" SETUP_AP_PWD "') and open http://192.168.4.1/");
  }
  Serial.println("Serial console ready. Try: identities | wifi | set wifi.ssid <x> | set wifi.pwd <y>");
  Serial.println("Target an identity with a prefix, e.g. 'room advert' or 'repeater set lat -37.81'.");
  Serial.print("> ");
}

// read a line from USB serial and run it through the same dispatcher the web
// panel uses, so the node is manageable over USB before WiFi is configured.
static char s_line[512];   // identity-restore blobs are ~214 chars
static size_t s_len = 0;

static void serviceSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (s_len > 0) {
        s_line[s_len] = 0;
        Serial.println();
        char reply[512];
        g_runner.runWebCommand(s_line, reply, sizeof(reply));
        Serial.println(reply[0] ? reply : "OK");
        s_len = 0;
        Serial.print("> ");
      }
    } else if (s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = c;
      Serial.print(c);   // echo
    }
  }
}

// main-task load: share of wall time spent working (vs parked in delay(1)),
// measured over 1-second windows. This is the composition's own duty cycle,
// not whole-chip utilisation (WiFi/BT stacks run on their own tasks).
static uint32_t s_busy_us = 0, s_load_win_ms = 0, s_loops = 0;
static volatile uint8_t  s_load_pct = 0;
static volatile uint32_t s_loops_per_s = 0;
uint8_t  multiLoadPct() { return s_load_pct; }
uint32_t multiLoopsPerSec() { return s_loops_per_s; }

void loop() {
  uint32_t t0 = micros();

  // pump each identity, then service the shared radio: pump() drains it into
  // the receive queue, which the identities take from on the next iteration
  for (int i = 0; i < NUM_MODULES; i++) g_modules[i]->loop();
  g_core->pump();

  if (g_setup_mode) {
    g_setup_srv.handleClient();   // SoftAP config portal
  } else {
    network.loop(true);           // composition keeps WiFi up for the panel
    web.loop();
  }
  serviceConsoleRequest();        // run web/serial commands in THIS task, not the server's
  serviceDeferredCall();          // ...and web reads of live mesh state, likewise
  applySlotRequests();            // hot enable/disable of chat identities
  gpsSyncTick();                  // scheduled GPS clock discipline
  multiWebTick();                 // stats history sampler (unified panel)
  serviceSerial();

  s_busy_us += micros() - t0;
  s_loops++;
  uint32_t now = millis();
  if (now - s_load_win_ms >= 1000) {
    uint32_t win_us = (now - s_load_win_ms) * 1000;
    s_load_pct = (uint8_t)min(100UL, (unsigned long)(s_busy_us * 100 / win_us));
    s_loops_per_s = s_loops;
    s_busy_us = 0; s_loops = 0; s_load_win_ms = now;
  }

  // Yield 1ms per pass so the FreeRTOS idle task runs (WFI clock-gates the
  // core) instead of busy-spinning at 100% — LoRa symbols are milliseconds,
  // so a 1ms poll interval costs nothing and saves tens of mA.
  delay(1);
}
