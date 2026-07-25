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

static IdentityModule* g_modules[] = { &repeater_module, &room_module, &companion_module };
static const int NUM_MODULES = sizeof(g_modules) / sizeof(g_modules[0]);

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
static SharedRadioCore* g_core = nullptr;
static RadioPort        port_rep, port_room, port_comp;

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
    if (strncmp(command, "set wifi.powersave ", 19) == 0) {
      if (network.setWifiPowerSave(command + 19)) {
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

// accessors for the unified web panel (web_multi.cpp)
SharedRadioCore* multiCore() { return g_core; }
void multiRunConsole(const char* cmd, char* reply, size_t reply_size) {
  g_runner.runWebCommand(cmd, reply, reply_size);
}
fs::FS* multiSysFS() { return &fs_sys; }
void multiGetRadioParams(float* freq, float* bw, uint8_t* sf, uint8_t* cr) {
  if (freq) *freq = g_radio.freq;
  if (bw)   *bw = g_radio.bw;
  if (sf)   *sf = g_radio.sf;
  if (cr)   *cr = g_radio.cr;
}

void halt() { while (1); }

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
  if (!fs_room.exists("/com_prefs")) cfg(&room_module,     "set name " ADVERT_NAME " Room");
  // location policy is a design constraint, applied every boot:
  cfg(&repeater_module, "gps advert prefs");     // repeater shares stored lat/lon
  cfg(&room_module,     "gps advert none");       // room never shares location
  // companion: BaseChatMesh, location off by default; name left at its default
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== MeshCore multi-identity ===");

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
  g_core->setRxErrorCounter([]() -> uint32_t { return radio_driver.getPacketsRecvErrors(); });
  g_core->setPortName(g_core->addPort(port_rep),  "repeater");
  g_core->setPortName(g_core->addPort(port_room), "room");
  g_core->setPortName(g_core->addPort(port_comp), "companion");

  WebPanelServer::setExtRoutesRegistrar(&multiWebRegisterRoutes);   // unified panel + APIs
  WebPanelServer::setExtOwnsIndex(true);                            // panel served at "/" (stock SPA stays at /app)

  repeater_module.setup(&fs_rep, &port_rep);
  room_module.setup(&fs_room, &port_room);
  companion_module.setup(&fs_comp, &port_comp);

  // authoritative last: overrides whatever each identity's own begin() just
  // applied to the shared radio_driver (see "shared-radio ownership" above)
  applyRadioParams(radioStoreGet());

  applyAdvertPolicy();

  loadAdminPwd();   // web/console admin password (persisted, defaults to ADMIN_PASSWORD)

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
static char s_line[160];
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

  // pump each identity, then fetch the next shared radio frame (order matters:
  // all ports must consume the current frame before pump() fetches the next)
  for (int i = 0; i < NUM_MODULES; i++) g_modules[i]->loop();
  g_core->pump();

  if (g_setup_mode) {
    g_setup_srv.handleClient();   // SoftAP config portal
  } else {
    network.loop(true);           // composition keeps WiFi up for the panel
    web.loop();
  }
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
