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
#include <sys/time.h>
#include <esp_sntp.h>
#include <target.h>
#include <helpers/SharedRadio.h>
#include <helpers/NetworkService.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/web/WebService.h>
#include "identity_module.h"
#include "multi_web.h"
#include "identity_backup.h"
#include "diag_service.h"
#include "slot_types.h"
#include "bot_api.h"
#ifdef WITH_MQTT_UPLINK
#include <helpers/mqtt/MQTTUplink.h>
#endif

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
  // the trace labels transmissions with the CR they went out at, so the arbiter
  // is told whenever it changes rather than reading it back off the chip
  if (g_core != nullptr) g_core->setCodingRate(p.cr);
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

// ---- clock source, shared by GPS and NTP ------------------------------------
// Both discipline the same RTC, so which one last set it — and when — has to be
// tracked in one place. Anything reporting the time (the panel, the public
// !time responder) needs to say where it came from and how stale that is; a
// clock nobody has checked in a day means something different from one checked
// a minute ago, even though both read plausibly.
static const char* g_clock_src = "unset";       // "GPS" | "NTP" | "unset"
static uint32_t g_clock_last_epoch = 0;         // epoch AT the last successful sync
static uint32_t g_clock_last_ms = 0;            // millis() at that moment

// ---- NTP -------------------------------------------------------------------
// The GPS receiver has never once achieved a fix on this node, while WiFi is up
// essentially all the time, so NTP is the primary source where it is available
// and GPS is the fallback for a deployment with no network. SNTP itself only
// sets the ESP32 SYSTEM clock; MeshCore timestamps everything from rtc_clock,
// so the value has to be copied across explicitly — and routed through the same
// drift bookkeeping as GPS so the crystal's error stays measurable.
static const uint32_t NTP_RETRY_MS = 15000;      // while waiting for a first answer
static uint32_t g_ntp_interval_h = 6;
static uint32_t g_ntp_next_ms = 20000;           // first attempt shortly after boot
static bool     g_ntp_started = false;
static uint32_t g_ntp_syncs = 0;
static const char* g_ntp_last_result = "not attempted yet";
static char g_ntp_server[64] = "pool.ntp.org";   // overridable: some LANs block outbound NTP

// ---- clock drift history ----------------------------------------------------
// Every GPS sync measures how far the RTC had wandered since the previous one.
// Kept in RTC slow memory: it survives OTA/software reboots (NOT a power cycle)
// and, unlike NVS, costs no flash writes — this is diagnostics, not state
// worth wearing flash for.
struct ClockDriftSample {
  uint32_t epoch;       // reference time at the sync
  int32_t  offset_ms;   // reference - rtc, MILLISECONDS (positive => RTC slow)
  uint32_t elapsed_s;   // since the previous sync (0 if this was the first)
  // How wide the poll gap was when the tick edge was caught. The edge could
  // have fallen anywhere inside it, so this IS the measurement uncertainty:
  // +/- gap/2. Normally a main-loop period (tens of ms); when the loop stalls
  // on radio or web work it can be most of a second, and that sample is then
  // worthless. Recording it is what makes a bad sample identifiable instead of
  // just looking like exciting drift.
  uint16_t gap_ms;
  uint8_t  subsec;      // 1 = phase measurement, 0 = whole-second fallback
};
// Bumped again for the gap/subsec fields — a stale history would be read with
// the new fields full of noise and silently rejected or trusted at random.
#define DRIFT_MAGIC 0x44524656u   // 'DRFV'
#define DRIFT_SLOTS 24
RTC_NOINIT_ATTR static uint32_t g_drift_magic;
RTC_NOINIT_ATTR static ClockDriftSample g_drift[DRIFT_SLOTS];
RTC_NOINIT_ATTR static uint32_t g_drift_count;    // total ever recorded

// A sample this far out is not drift, it is an event — a reboot with a stale
// clock, a botched sync, a garbage RTC read. Averaging those in would poison
// the rate estimate that the trimmer below acts on, so they are kept in the
// history (they are worth seeing) but excluded from every calculation.
#define DRIFT_OUTLIER_PPM 1000.0f

// A sample also needs a long enough baseline to mean anything. The phase
// measurement carries a residual error of roughly half a main-loop period —
// tens of milliseconds — and dividing that by a short interval manufactures an
// enormous rate from nothing: a real +21 ms measurement over 22 s reads as
// +954 ppm. Half an hour keeps that artefact under ~1 ppm, which is below the
// drift being measured. Short samples are still recorded and shown; they just
// do not vote on the rate.
#define DRIFT_MIN_INTERVAL_S 1800u

// A sample measured across a stalled loop is not a measurement. The observed
// bad samples were all of this kind — +483 ms, -217 ms, +142 ms, every one a
// sub-second reading rather than a whole-second fallback, i.e. the edge was
// caught late by a loop that had gone away for a while. At an hourly cadence a
// 250 ms gap is already +/-35 ppm of uncertainty on a 24 ppm signal, so
// anything wider is discarded rather than averaged in.
#define DRIFT_MAX_GAP_MS 250u

static void driftInit() {
  if (g_drift_magic != DRIFT_MAGIC) {
    memset(g_drift, 0, sizeof(g_drift));
    g_drift_count = 0;
    g_drift_magic = DRIFT_MAGIC;
  }
}

static void driftRecord(uint32_t ref_epoch, int32_t offset_ms, uint32_t elapsed_s,
                        uint16_t gap_ms, bool subsec) {
  driftInit();
  g_drift[g_drift_count % DRIFT_SLOTS] =
      { ref_epoch, offset_ms, elapsed_s, gap_ms, (uint8_t)(subsec ? 1 : 0) };
  g_drift_count++;
}

static float driftSamplePpm(const ClockDriftSample& s) {
  if (s.elapsed_s == 0) return 0;
  return (float)s.offset_ms * 1000.0f / (float)s.elapsed_s;
}

// parts-per-million from the most recent measured interval (0 if unknown)
static float driftPpm() {
  driftInit();
  if (g_drift_count == 0) return 0;
  return driftSamplePpm(g_drift[(g_drift_count - 1) % DRIFT_SLOTS]);
}

// Mean rate over the retained history, outliers dropped. This — not the last
// interval — is what a correction should be based on: one interval carries the
// full measurement error of two endpoint readings, while the mean over a day
// of six-hourly samples averages that down.
// ---- drift correction -------------------------------------------------------
// Knowing the rate makes it correctable. The RTC cannot be slewed the way ntpd
// slews a host clock — every backend's setCurrentTime() takes whole seconds —
// so instead the predicted accumulated error is tracked continuously and the
// clock stepped by one second each time that prediction crosses a whole one.
//
// Between six-hourly NTP syncs this is the difference between drifting a second
// or two and staying inside half of one. For a deployment with no network,
// where the next reference might be days away, it is the difference between
// seconds and minutes — and it is the only correction available there at all.
//
// Trimming is refused unless a real hardware RTC is present: on the fallback
// path getCurrentTime() IS the system clock that SNTP disciplines, so there is
// no independent oscillator to correct and "drift" is definitionally zero.
static bool     g_trim_enabled = true;
static int32_t  g_trim_applied_ms = 0;    // steps applied since the last sync
static uint32_t g_trim_next_ms = 0;
// What the last whole-second correction left in the clock (the chip's tick
// phase). Removed from the next measurement so it is not billed as drift.
static double   g_phase_residual_ms = 0;
// Latched at boot: 1 = the RTC's oscillator had stopped since it was last
// set (so its time was meaningless, not merely drifted), 0 = ran clean,
// -1 = the chip cannot report it. See the sample in setup().
static int      g_rtc_osc_stopped = -1;
static uint32_t g_trim_steps = 0;         // total ever applied, for reporting

// Is this sample fit to vote on the rate?
static bool driftSampleUsable(const ClockDriftSample& s) {
  if (s.elapsed_s < DRIFT_MIN_INTERVAL_S) return false;
  if (!s.subsec) return false;              // whole-second fallback: 278 ppm quantum at 1 h
  if (s.gap_ms > DRIFT_MAX_GAP_MS) return false;
  float ppm = driftSamplePpm(s);
  return !(ppm > DRIFT_OUTLIER_PPM || ppm < -DRIFT_OUTLIER_PPM);
}

// MEDIAN rate over the retained history, not the mean.
//
// This is not fastidiousness. Measured overnight: ten samples clustered inside
// 23.6-24.2 ppm and five strays at -60, +37, +37, +39, +134. The mean of that
// is 28.8 ppm; the median is 23.9. The trimmer acts on this number, so a mean
// would have had it over-correcting by a fifth, forever, on the strength of a
// handful of samples taken while the loop was busy elsewhere. A median cannot
// be dragged that way — it does not care how wrong a minority is, only how
// many of them there are.
static float driftPpmEstimate(int* n_used = nullptr) {
  driftInit();
  uint32_t shown = g_drift_count < DRIFT_SLOTS ? g_drift_count : DRIFT_SLOTS;
  uint32_t start = g_drift_count - shown;
  float v[DRIFT_SLOTS];
  int n = 0;
  for (uint32_t i = start; i < g_drift_count; i++) {
    const ClockDriftSample& s = g_drift[i % DRIFT_SLOTS];
    if (!driftSampleUsable(s)) continue;
    v[n++] = driftSamplePpm(s);
  }
  if (n_used) *n_used = n;
  if (n == 0) return 0;
  // insertion sort; n <= 24 and this runs about once a minute
  for (int i = 1; i < n; i++) {
    float k = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

// The M5 has a physical GPS slide switch on its side wired to PIN_GPS_SWITCH.
// The variant configures the pin as an input and then never reads it, so the
// firmware has been blind to it: with the switch off the module is dead at the
// hardware level and no amount of software enabling brings it back. That is
// indistinguishable from "no sky view" unless the pin is checked — the M1
// variant reads the same switch and documents HIGH as ON, so follow that.
static bool gpsSwitchOn() {
#ifdef PIN_GPS_SWITCH
  return digitalRead(PIN_GPS_SWITCH) == HIGH;
#else
  return true;      // no switch on this board: nothing to veto
#endif
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

// ---- sub-second RTC error ---------------------------------------------------
// getCurrentTime() is whole seconds on every backend, so differencing it
// against a reference can only ever yield an integer — and at a six-hour sync
// interval one second of quantisation IS 46 ppm, larger than the drift of any
// crystal worth measuring. That is why this node's entire recorded history
// reads +1s or +2s: the number was the quantum, not the clock.
//
// The sub-second information lives in the PHASE of the RTC's tick rather than
// its value. Watch getCurrentTime() until it steps from S to S+1: at that
// instant the RTC reads exactly S+1.000, and whatever the SNTP-disciplined
// microsecond system clock says at the same instant is the truth to compare it
// against. Resolution becomes the polling interval instead of a whole second.
//
// Polling is spread across main-loop passes. A second of blocking I2C reads
// would stall the radio for the whole of a LoRa frame, and the mesh should not
// pay that twice a day for a diagnostic.
static const uint32_t RTCP_POLL_MS   = 2;      // between reads; also the resolution
static const uint32_t RTCP_WINDOW_MS = 1500;   // a tick MUST fall inside 1s; this is the give-up
static bool     g_rtcp_active = false;
static bool     g_rtcp_done = false;           // an edge was caught
static uint32_t g_rtcp_deadline_ms = 0, g_rtcp_next_poll_ms = 0, g_rtcp_prev_sec = 0;
static uint32_t g_rtcp_last_poll_ms = 0;       // when the PREVIOUS poll ran
static uint32_t g_rtcp_gap_ms = 0;             // the gap the edge was caught in
static double   g_rtcp_error_ms = 0;           // reference - rtc, at the edge

static void rtcPhaseStart() {
  g_rtcp_active = true;
  g_rtcp_done = false;
  g_rtcp_prev_sec = rtc_clock.getCurrentTime();
  g_rtcp_next_poll_ms = millis();
  g_rtcp_last_poll_ms = millis();
  g_rtcp_gap_ms = 0;
  g_rtcp_deadline_ms = millis() + RTCP_WINDOW_MS;
}

// true once the sampler has finished — edge caught, or window expired
static bool rtcPhaseTick() {
  if (!g_rtcp_active) return true;
  uint32_t now = millis();
  if ((int32_t)(now - g_rtcp_next_poll_ms) < 0) return false;
  // The poll cadence is really the main-loop period, which on this node is tens
  // of milliseconds under load rather than the RTCP_POLL_MS we ask for. Record
  // the gap actually achieved so the edge can be placed inside it.
  g_rtcp_gap_ms = now - g_rtcp_last_poll_ms;
  g_rtcp_last_poll_ms = now;
  g_rtcp_next_poll_ms = now + RTCP_POLL_MS;

  uint32_t s = rtc_clock.getCurrentTime();
  if (s != g_rtcp_prev_sec) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    double ref = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    // positive => the reference is ahead of the RTC, i.e. the RTC is running slow
    g_rtcp_error_ms = (ref - (double)s) * 1000.0;
    g_rtcp_done = true;
    g_rtcp_active = false;
    return true;
  }
  if ((int32_t)(now - g_rtcp_deadline_ms) >= 0) { g_rtcp_active = false; return true; }
  return false;
}

// Has SNTP genuinely answered, as opposed to the system clock merely looking
// plausible? This must be authoritative, because the failure it guards against
// is subtle: ESP32RTCClock restores a persisted epoch from NVS at boot, which
// can be a quarter of an hour stale and still passes any "is this a sane date"
// test. Adopting it as an NTP answer writes that staleness into the RTC and
// then reports a clean sync — which is how this node came to be half an hour
// out while claiming it had synced 1.4 h earlier.
//
// sntp_get_sync_status() CANNOT be used for this. IDF clears the COMPLETED
// status on read, so it is a one-shot consumed by whoever polls first — and
// NetworkService::updateTimeSync() polls it continuously on its own loop, so
// this code loses that race essentially every time. The notification callback
// is a dedicated slot nothing else here registers for, and it fires on every
// successful update.
static volatile bool g_sntp_answered_cb = false;
static void sntpSyncNotify(struct timeval*) { g_sntp_answered_cb = true; }
static bool g_ntp_answered = false;
static bool g_ntp_measuring = false;
static uint32_t g_ntp_started_ms = 0;
// How long to insist on a confirmed SNTP status before falling back to the
// date test alone (see the escape hatch in ntpSyncTick).
static const uint32_t NTP_STATUS_GRACE_MS = 15UL * 60UL * 1000UL;
// Above kDefaultEpoch (ESP32RTCClock seeds an unset clock to 15 May 2024), so
// the node's own placeholder can never be mistaken for an answer. See the
// comment at the sanity gate below.
static const time_t NTP_MIN_SANE_EPOCH = 1735689600L;   // 2025-01-01

// Fold a completed measurement into the drift history and correct the clock.
static void ntpApplySync(uint32_t now) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  uint32_t sys = (uint32_t)tv.tv_sec;

  double raw_ms;
  if (g_rtcp_done) {
    // The edge fell somewhere inside the gap between the last two polls, and
    // all we know is that it had happened by the end of it. The midpoint is the
    // unbiased estimate. Using the gap ACTUALLY achieved rather than the one we
    // asked for matters: a hardcoded 1 ms correction against a ~40 ms loop
    // period leaves a systematic +20 ms, which at an hourly sync cadence is a
    // phantom 6 ppm of drift — the same order as the real thing.
    raw_ms = g_rtcp_error_ms - (double)g_rtcp_gap_ms / 2.0;
  } else {
    // No edge inside the window (a very slow RTC read). Still record something
    // rather than silently skipping the sync — just at the old resolution.
    raw_ms = ((double)sys - (double)rtc_clock.getCurrentTime()) * 1000.0;
  }

  // Add back whatever the trimmer already corrected during this interval, or
  // the rate estimate would be measuring the trim rather than the crystal —
  // the RTC would look perfect, the rate would fall to zero, and the trim
  // would switch itself off.
  // Subtract the residual the LAST correction left behind. Corrections are
  // whole seconds, so each one leaves the chip's tick phase — about +48 ms
  // here — sitting in the clock. That residual then turns up in full in the
  // next measurement, and dividing it by the interval charges it to the
  // crystal: at an hourly cadence a constant 48 ms reads as 13 ppm of drift
  // that does not exist. The first hour-long sample measured +38.89 ppm; the
  // real rate underneath it is nearer 25.
  //
  // No extra measurement is needed to remove it. After correcting by corr
  // seconds the residual is exactly raw - corr*1000, so remembering that and
  // subtracting it next time leaves only what the crystal actually did.
  int32_t offset_ms = (int32_t)(raw_ms - g_phase_residual_ms + (double)g_trim_applied_ms);
  uint32_t elapsed = g_clock_last_epoch ? (sys - g_clock_last_epoch) : 0;
  // Recorded unconditionally. The previous code stored a sample only when the
  // whole-second offset was non-zero, which threw away every interval where the
  // clock was RIGHT and left an average computed purely from the bad ones.
  driftRecord(sys, offset_ms, elapsed,
              (uint16_t)(g_rtcp_done ? g_rtcp_gap_ms : 0), g_rtcp_done);

  // Correct by WHOLE SECONDS, and only when a whole second is actually owed.
  //
  // The PCF8563 free-runs its divider: writing the seconds register changes the
  // counter but does not move the tick edge, which on this chip sits ~52 ms
  // after the true second and stays there. So the sub-second phase is a
  // property of the hardware that no write can fix, and any attempt to fix it
  // makes things worse — writing at an arbitrary moment injected a uniform
  // 0-1000 ms error (measured: +793 ms), and writing on the reference's second
  // boundary injected a consistent -948 ms, leaving the clock a full second
  // FAST for 95% of every second.
  //
  // What is left is to get the integer right and leave the phase alone. This
  // runs immediately after the sampler caught the chip's own tick edge, so
  // rounding the measured error to whole seconds and adding that puts the
  // counter on the nearest correct second; the residual is the chip's phase,
  // bounded by half a second and in practice ~50 ms. When nothing is owed —
  // the normal case once synced — nothing is written at all.
  int32_t corr = (int32_t)llround(raw_ms / 1000.0);
  if (corr != 0) rtc_clock.setCurrentTime(rtc_clock.getCurrentTime() + corr);
  g_phase_residual_ms = raw_ms - (double)corr * 1000.0;

  g_trim_applied_ms = 0;
  g_clock_src = "NTP";
  g_clock_last_epoch = sys;
  g_clock_last_ms = now;
  g_ntp_syncs++;
  g_ntp_last_result = "synced";
  Serial.printf("[ntp] RTC was %+ld ms out over %lu s%s; corrected %+ld s\n",
                (long)offset_ms, (unsigned long)elapsed,
                g_rtcp_done ? "" : " (whole-second fallback)", (long)corr);
  g_ntp_next_ms = now + g_ntp_interval_h * 3600000UL;
}

static void ntpSyncTick() {
  if (WiFi.status() != WL_CONNECTED) {
    if (g_ntp_started) {
      g_ntp_started = false; g_ntp_answered = false;
      g_ntp_measuring = false;
      g_ntp_last_result = "waiting for wifi";
    }
    return;
  }
  uint32_t now = millis();
  if (!g_ntp_started) {
    // Start (or restart after a reconnect) the SNTP client. Two public fallbacks
    // behind the configured server so a single unreachable host isn't fatal.
    configTzTime("UTC0", g_ntp_server, "time.cloudflare.com", "time.google.com");
    sntp_set_time_sync_notification_cb(sntpSyncNotify);
    g_ntp_started = true;
    // g_sntp_answered_cb is deliberately NOT cleared here: it means "SNTP has
    // answered at some point this boot", and after a wifi blip the next update
    // can be an hour away. Clearing it would strand the clock in the grace
    // period for no reason.
    g_ntp_answered = false;
    g_ntp_started_ms = now;
    g_ntp_last_result = "querying...";
    g_ntp_next_ms = now + NTP_RETRY_MS;
    return;
  }

  // A measurement is in progress: let the phase sampler run to completion
  // before touching the clock, since correcting it would destroy the very
  // error we are trying to read.
  if (g_ntp_measuring) {
    if (!rtcPhaseTick()) return;
    g_ntp_measuring = false;
    ntpApplySync(now);
    return;
  }

  if (now < g_ntp_next_ms) return;

  // Has SNTP actually answered? Asking the system clock alone is not enough: an
  // ESP32 that has never been set still reports a plausible epoch (May 2024),
  // which passed the old "is this a sane date" test and was adopted as truth.
  // This node's own history contains two such samples — each one set the whole
  // node's clock back two years until the following sync undid it.
  if (!g_ntp_answered) {
    if (g_sntp_answered_cb) {
      g_ntp_answered = true;
    } else if ((uint32_t)(now - g_ntp_started_ms) < NTP_STATUS_GRACE_MS) {
      g_ntp_last_result = "no answer yet";
      g_ntp_next_ms = now + NTP_RETRY_MS;
      return;
    } else {
      // Escape hatch. The status flag is the only thing that can tell a real
      // answer from a stale-but-plausible clock (ESP32RTCClock restores a
      // persisted epoch on boot, which is recent enough to pass any date
      // test). But if it never arrives — a platform quirk, a firewalled
      // server — refusing to ever set the clock is the worse failure, so
      // after the grace period fall through on the date test alone and say so.
      g_ntp_last_result = "synced (unconfirmed: no SNTP status)";
    }
  }
  if (time(nullptr) < NTP_MIN_SANE_EPOCH) {
    g_ntp_last_result = "answer failed sanity check";
    g_ntp_next_ms = now + NTP_RETRY_MS;
    return;
  }

  rtcPhaseStart();
  g_ntp_measuring = true;
}


#ifdef WITH_MQTT_UPLINK
// ---- MQTT uplink -----------------------------------------------------------
// The uplink authenticates with a JWT signed by the node's OWN mesh identity —
// no shared secret. The broker verifies the Ed25519 signature against the public
// key carried in the token payload, so the thing that proves who we are on the
// mesh also proves who we are to the broker. The custom-broker slot bypasses
// that and uses plain username/password instead.
//
// The repeater identity is the one used: it is the role this node presents to
// the mesh, so its key is the honest answer to "who is publishing this".
static mesh::LocalIdentity g_mqtt_id;
static MQTTUplink* g_mqtt = nullptr;
static char g_mqtt_name[40] = {0};

// Mirrors MQTTUplink's private broker bits (MQTTUplink.h) — they are not
// exported, so the values are repeated here rather than reached into.
static const struct { const char* key; uint8_t bit; } MQTT_BROKERS[] = {
  {"eastmesh-au", 0x01}, {"letsmesh-eu", 0x02}, {"letsmesh-us", 0x04},
  {"custom", 0x08}, {"meshmapper", 0x10}, {"waev", 0x20},
};
static const int MQTT_BROKER_COUNT = sizeof(MQTT_BROKERS) / sizeof(MQTT_BROKERS[0]);

static uint8_t mqttBrokerBit(const char* key) {
  for (int i = 0; i < MQTT_BROKER_COUNT; i++) {
    if (strcasecmp(key, MQTT_BROKERS[i].key) == 0) return MQTT_BROKERS[i].bit;
  }
  return 0;
}

// Publish each frame the shared radio handles. The uplink wants mesh::Packet,
// the arbiter only has raw bytes, so the frame is parsed here — a single static
// Packet is safe because the hook only ever runs on the loop task, from pump()
// and tryStartSend().
//
// Gated on the uplink's own packets setting, so turning it off actually stops
// the work rather than just discarding the result.
// Counters so the wiring is verifiable from the outside: "connected" says
// nothing about whether frames are reaching the uplink, and the uplink's own
// publish path is silent unless built with MQTT_DEBUG.
static uint32_t g_mqtt_seen = 0, g_mqtt_parsed = 0, g_mqtt_pub = 0;

static void mqttFrameHook(const uint8_t* f, int len, bool is_tx, float snr, float rssi) {
  g_mqtt_seen++;
  if (g_mqtt == nullptr || !g_mqtt->isPacketsEnabled()) return;
  if (f == nullptr || len <= 0 || len > 255) return;      // readFrom takes a uint8_t length
  static mesh::Packet pkt;
  if (!pkt.readFrom(f, (uint8_t)len)) return;             // not a frame we can parse
  g_mqtt_parsed++;
  g_mqtt->publishPacket(pkt, is_tx, (int)rssi, snr, -1,
                        g_core ? (int)g_core->real()->getEstAirtimeFor(len) : -1);
  g_mqtt_pub++;
}

// Started from the LOOP, not setup(): it needs WiFi anyway, and anything that
// faults before network.begin() costs remote access entirely (see botTick).
static void mqttTick() {
  static bool inited = false;
  static uint32_t next_ms = 0;
  if (!inited) {
    inited = true;
    if (!multiIdLoad("repeater", &fs_rep, g_mqtt_id)) {
      Serial.println("[mqtt] no repeater identity — uplink not started");
      return;
    }
    { char reply[80]; reply[0] = 0;
      repeater_module.run_command("get name", reply, sizeof(reply));
      const char* n = reply[0] == '>' ? reply + 1 : reply;
      while (*n == ' ') n++;
      StrHelper::strncpy(g_mqtt_name, n, sizeof(g_mqtt_name)); }
    g_mqtt = new MQTTUplink(rtc_clock, g_mqtt_id);
    g_mqtt->setNodeNameSource(g_mqtt_name);
    g_mqtt->setNetworkStateProvider(&network);
    // Prefs go to the SPIFFS ROOT, not the /fs/sys SubdirFS view. MQTTPrefsStore
    // does exists()+remove() before writing, and through the subdirectory
    // wrapper that sequence does not survive — the value applied in RAM and was
    // silently lost on the next boot, with the setter reporting failure while
    // the readback showed it set. One file at the root avoids the whole
    // question.
    g_mqtt->begin(&fs_shared);
    if (g_core) g_core->setFrameHook(&mqttFrameHook);
    Serial.printf("[mqtt] uplink %s (node %s)\n",
                  g_mqtt->isActive() ? "enabled" : "idle (no broker selected)", g_mqtt_name);
  }
  if (g_mqtt == nullptr) return;
  uint32_t now = millis();
  if (now < next_ms) return;
  next_ms = now + 1000;      // the uplink schedules its own work; once a second is plenty

  MQTTStatusSnapshot st{};
  st.battery_mv = (int)board.getBattMilliVolts();
  st.uptime_secs = now / 1000;
  st.noise_floor = g_core ? (int)g_core->real()->getNoiseFloor() : 0;
  st.recv_errors = radio_driver.getPacketsRecvErrors();
  st.packets_sent = radio_driver.getPacketsSent();
  st.packets_received = radio_driver.getPacketsRecv();
  st.radio_freq = g_radio.freq;
  st.radio_bw = g_radio.bw;
  st.radio_sf = g_radio.sf;
  st.radio_cr = g_radio.cr;
  st.repeat_enabled = true;
  g_mqtt->loop(st);
}
#endif

// Step the RTC toward where the measured rate says it should be. Runs from the
// main loop; the predicted error moves by microseconds a second, so once a
// minute is far more often than it can possibly matter.
static void clockTrimTick() {
  uint32_t now = millis();
  if (now < g_trim_next_ms) return;
  g_trim_next_ms = now + 60000;

  if (!g_trim_enabled) return;
  if (!AutoDiscoverRTCClock::hasHardwareRTC()) return;   // nothing independent to trim
  if (g_clock_last_ms == 0) return;                      // never disciplined, no baseline
  // Never step the clock while the phase sampler is hunting for a tick edge —
  // it would land inside the measurement and be read back as drift.
  if (g_rtcp_active || g_ntp_measuring) return;

  int n = 0;
  float ppm = driftPpmEstimate(&n);
  if (n < 3) return;                        // one or two intervals is not a rate
  if (ppm > -1.0f && ppm < 1.0f) return;    // inside the measurement noise; leave it alone

  double elapsed_s = (double)(now - g_clock_last_ms) / 1000.0;
  double predicted_ms = elapsed_s * (double)ppm / 1000.0;    // ppm x seconds = µs
  double outstanding = predicted_ms - (double)g_trim_applied_ms;
  if (outstanding > -1000.0 && outstanding < 1000.0) return;  // less than a step's worth

  int32_t step = outstanding > 0 ? 1 : -1;   // positive ppm => RTC slow => add time
  rtc_clock.setCurrentTime(rtc_clock.getCurrentTime() + step);
  g_trim_applied_ms += step * 1000;
  g_trim_steps++;
  Serial.printf("[clock] trim %+ld s (rate %.2f ppm from %d samples, %.0f ms outstanding)\n",
                (long)step, (double)ppm, n, outstanding);
}

static void gpsSyncStart() {
  gpsPower(true);
  g_gps_deadline_ms = millis() + GPS_FIX_TIMEOUT_MS;
  g_gps_search_start_ms = millis();
  g_gps_last_result = "acquiring fix...";
}

static void gpsSyncTick() {
  uint32_t now = millis();

  if (!gpsSwitchOn()) {
    // Hardware switch is off. Nothing can be received, so do not sit there
    // burning receiver current pretending to search.
    if (g_gps_deadline_ms != 0) {
      gpsPower(false);
      g_gps_deadline_ms = 0;
      g_gps_search_start_ms = 0;
    }
    g_gps_last_result = "hardware switch is OFF";
    return;
  }

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
      // the interval since the last sync, is the crystal's real drift rate.
      // NMEA only carries whole seconds, so unlike the NTP path this one has no
      // sub-second reference to phase-compare against and stays quantised.
      uint32_t before = rtc_clock.getCurrentTime();
      int32_t offset_ms = (int32_t)((uint32_t)ts - before) * 1000 + g_trim_applied_ms;
      uint32_t elapsed = g_clock_last_epoch ? ((uint32_t)ts - g_clock_last_epoch) : 0;
      driftRecord((uint32_t)ts, offset_ms, elapsed, 0, false);   // NMEA is whole seconds

      rtc_clock.setCurrentTime((uint32_t)ts);
      g_trim_applied_ms = 0;
      g_phase_residual_ms = 0;   // whole-second GPS write: phase now unknown
      g_gps_last_sync_epoch = (uint32_t)ts;
      g_clock_src = "GPS";
      g_clock_last_epoch = (uint32_t)ts;
      g_clock_last_ms = millis();
      g_gps_last_result = "synced from GPS";
      if (elapsed) {
        Serial.printf("[gps] clock synced to %lu (RTC was %+ld ms over %lu s = %.1f ppm)\n",
                      (unsigned long)ts, (long)offset_ms, (unsigned long)elapsed,
                      (double)offset_ms * 1000.0 / (double)elapsed);
      } else {
        Serial.printf("[gps] clock synced to %lu (first sync, RTC was %+ld ms out)\n",
                      (unsigned long)ts, (long)offset_ms);
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
    // ---- MQTT uplink ----
#ifdef WITH_MQTT_UPLINK
    if (strcmp(command, "mqtt") == 0) {
      if (g_mqtt == nullptr) { snprintf(reply, reply_size, "mqtt: not started yet"); return; }
      size_t o = 0;
      g_mqtt->formatStatusReply(reply, reply_size);
      o = strlen(reply);
      o += snprintf(reply + o, reply_size - o, "\nbrokers:");
      for (int i = 0; i < MQTT_BROKER_COUNT && o + 32 < reply_size; i++) {
        o += snprintf(reply + o, reply_size - o, " %s=%s", MQTT_BROKERS[i].key,
                      g_mqtt->isEndpointEnabled(MQTT_BROKERS[i].bit) ? "on" : "off");
      }
      o += snprintf(reply + o, reply_size - o,
                    "\nframes: seen=%lu parsed=%lu published=%lu",
                    (unsigned long)g_mqtt_seen, (unsigned long)g_mqtt_parsed,
                    (unsigned long)g_mqtt_pub);
      o += snprintf(reply + o, reply_size - o,
                    "\ncustom: host=%s port=%u transport=%s user=%s pass=%s",
                    g_mqtt->getCustomHost()[0] ? g_mqtt->getCustomHost() : "-",
                    (unsigned)g_mqtt->getCustomPort(), g_mqtt->getCustomTransport(),
                    g_mqtt->getCustomUsername()[0] ? g_mqtt->getCustomUsername() : "-",
                    g_mqtt->hasCustomPassword() ? "set" : "-");
      return;
    }
    if (strncmp(command, "set mqtt.", 9) == 0) {
      if (g_mqtt == nullptr) { snprintf(reply, reply_size, "Error - mqtt not started yet"); return; }
      const char* k = command + 9;
      const char* v = strchr(k, ' ');
      if (v == nullptr) { snprintf(reply, reply_size, "Error - usage: set mqtt.<key> <value>"); return; }
      char key[16];
      size_t klen = (size_t)(v - k);
      if (klen >= sizeof(key)) klen = sizeof(key) - 1;
      memcpy(key, k, klen); key[klen] = 0;
      v++;
      bool ok = false;
      if      (strcmp(key, "host") == 0)      ok = g_mqtt->setCustomHost(v);
      else if (strcmp(key, "port") == 0)      ok = g_mqtt->setCustomPort(v);
      else if (strcmp(key, "user") == 0)      ok = g_mqtt->setCustomUsername(v);
      else if (strcmp(key, "pass") == 0)      ok = g_mqtt->setCustomPassword(v);
      else if (strcmp(key, "transport") == 0) {
        // the API speaks "tcp"/"wss"; the panel's select and older habits use 0/1
        const char* t = v;
        if (strcmp(v, "0") == 0) t = "tcp";
        else if (strcmp(v, "1") == 0) t = "wss";
        ok = g_mqtt->setCustomTransport(t);
        if (!ok && strcmp(t, "wss") == 0) {
          // wss for a CUSTOM broker needs the mbedTLS certificate bundle, which
          // this build does not ship (the curated brokers carry their own PEMs).
          snprintf(reply, reply_size,
                   "Error - wss needs the CA certificate bundle, not built in; use tcp");
          return;
        }
      }
      else if (strcmp(key, "iata") == 0)      ok = g_mqtt->setIata(v);
      else if (strcmp(key, "owner") == 0)     ok = g_mqtt->setOwnerPublicKey(v);
      else if (strcmp(key, "email") == 0)     ok = g_mqtt->setOwnerEmail(v);
      else if (strcmp(key, "packets") == 0)   ok = g_mqtt->setPacketsEnabled(strcmp(v, "on") == 0);
      else if (strcmp(key, "status") == 0)    ok = g_mqtt->setStatusEnabled(strcmp(v, "on") == 0);
      else if (strcmp(key, "broker") == 0) {
        // "set mqtt.broker <key> on|off"
        char bk[20]; const char* sp = strchr(v, ' ');
        if (sp == nullptr) { snprintf(reply, reply_size, "Error - usage: set mqtt.broker <name> on|off"); return; }
        size_t bl = (size_t)(sp - v); if (bl >= sizeof(bk)) bl = sizeof(bk) - 1;
        memcpy(bk, v, bl); bk[bl] = 0;
        uint8_t bit = mqttBrokerBit(bk);
        if (bit == 0) { snprintf(reply, reply_size, "Error - unknown broker '%s'", bk); return; }
        ok = g_mqtt->setEndpointEnabled(bit, strcmp(sp + 1, "on") == 0);
      } else { snprintf(reply, reply_size, "Error - unknown mqtt setting '%s'", key); return; }
      // The password is deliberately not echoed back.
      // The bool from these setters means "persisted", not "accepted" — saying
      // "rejected" when the value is live but unsaved is worse than useless.
      snprintf(reply, reply_size, ok ? "OK - mqtt.%s saved"
                                     : "Error - mqtt.%s not saved (rejected, or the write failed)", key);
      return;
    }
#endif
    // ---- clock / GPS time discipline ----
    if (strcmp(command, "time") == 0 || strcmp(command, "clock") == 0) {
      uint32_t now_epoch = rtc_clock.getCurrentTime();
      LocationProvider* gps = sensors.getLocationProvider();
      char now_s[24], sync_s[24];
      formatEpochUtc(now_epoch, now_s, sizeof(now_s));
      if (g_gps_last_sync_epoch) formatEpochUtc(g_gps_last_sync_epoch, sync_s, sizeof(sync_s));
      else strncpy(sync_s, "never", sizeof(sync_s));
      int navg = 0;
      float ppm = driftPpmEstimate(&navg);
      snprintf(reply, reply_size,
               "epoch=%lu utc=%s source=%s rtc=%s last_gps_sync=%s gps=%s sats=%ld "
               "next_sync_in=%lus every=%luh batt=%umV drift=%.2fppm(%.2fs/day) median of %d usable "
               "last=%.2fppm trim=%s(%lu steps) osc_stopped=%s syncs=%lu skipped_low_batt=%lu",
               (unsigned long)now_epoch, now_s,
               g_gps_last_sync_epoch ? "gps" : "manual/unset",
               AutoDiscoverRTCClock::deviceName(), sync_s,
               g_gps_deadline_ms ? "on(acquiring)" : "off",
               gps ? gps->satellitesCount() : 0,
               (unsigned long)(g_gps_sync_hours == 0 ? 0 :
                 (g_gps_next_ms > millis() ? (g_gps_next_ms - millis()) / 1000 : 0)),
               (unsigned long)g_gps_sync_hours, (unsigned)board.getBattMilliVolts(),
               (double)ppm, (double)ppm * 86400.0 / 1000000.0, navg,
               (double)driftPpm(),
               !AutoDiscoverRTCClock::hasHardwareRTC() ? "n/a (no hardware RTC)"
                 : (g_trim_enabled ? "on" : "off"),
               (unsigned long)g_trim_steps,
               g_rtc_osc_stopped < 0 ? "unknown" : (g_rtc_osc_stopped ? "YES" : "no"),
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
      uint32_t oldest = g_drift_count - shown;

      // Print the NEWEST rows that fit, not the oldest. The loop used to start
      // at the beginning and run out of buffer, silently dropping the most
      // recent samples — the only ones anybody reads this for. Row widths vary
      // (an "(excluded)" note nearly doubles one), so rather than assume a
      // width, measure each row with snprintf(NULL, 0, ...) and walk backwards
      // from the newest until the budget is spent.
      const size_t footer = 96;      // the trailing "mean ..." line
      size_t budget = reply_size - footer - 56 /* header */;
      uint32_t start = g_drift_count;
      while (start > oldest) {
        const ClockDriftSample& s = g_drift[(start - 1) % DRIFT_SLOTS];
        char ts[24];
        formatEpochUtc(s.epoch, ts, sizeof(ts));
        int need;
        if (s.elapsed_s) {
          float ppm = driftSamplePpm(s);
          char why[48]; why[0] = 0;
          if (s.elapsed_s < DRIFT_MIN_INTERVAL_S) strcpy(why, "  (skip: baseline)");
          else if (!s.subsec) strcpy(why, "  (skip: whole-second)");
          else if (s.gap_ms > DRIFT_MAX_GAP_MS) snprintf(why, sizeof(why), "  (skip: %ums stall)", s.gap_ms);
          else if (ppm > DRIFT_OUTLIER_PPM || ppm < -DRIFT_OUTLIER_PPM) strcpy(why, "  (skip: outlier)");
          need = snprintf(nullptr, 0, "%s %+9ldms %6lumin %4ums %+.2f ppm%s\n",
                          ts, (long)s.offset_ms, (unsigned long)(s.elapsed_s / 60),
                          s.gap_ms, (double)ppm, why);
        } else {
          need = snprintf(nullptr, 0, "%s %+9ldms       -   (first)\n", ts, (long)s.offset_ms);
        }
        if (need < 0 || (size_t)need > budget) break;
        budget -= (size_t)need;
        start--;
      }
      if (start > oldest) {
        o += snprintf(reply + o, reply_size - o, "(newest %lu of %lu)\n",
                      (unsigned long)(g_drift_count - start), (unsigned long)g_drift_count);
      }
      o += snprintf(reply + o, reply_size - o, "utc                    rtc_error  interval  gap    rate\n");
      for (uint32_t i = start; i < g_drift_count; i++) {
        const ClockDriftSample& s = g_drift[i % DRIFT_SLOTS];
        char ts[24];
        formatEpochUtc(s.epoch, ts, sizeof(ts));
        if (s.elapsed_s) {
          float ppm = driftSamplePpm(s);
          char why[48]; why[0] = 0;
          if (s.elapsed_s < DRIFT_MIN_INTERVAL_S) strcpy(why, "  (skip: baseline)");
          else if (!s.subsec) strcpy(why, "  (skip: whole-second)");
          else if (s.gap_ms > DRIFT_MAX_GAP_MS) snprintf(why, sizeof(why), "  (skip: %ums stall)", s.gap_ms);
          else if (ppm > DRIFT_OUTLIER_PPM || ppm < -DRIFT_OUTLIER_PPM) strcpy(why, "  (skip: outlier)");
          o += snprintf(reply + o, reply_size - o, "%s %+9ldms %6lumin %4ums %+.2f ppm%s\n",
                        ts, (long)s.offset_ms, (unsigned long)(s.elapsed_s / 60),
                        s.gap_ms, (double)ppm, why);
        } else {
          o += snprintf(reply + o, reply_size - o, "%s %+9ldms       -   (first)\n",
                        ts, (long)s.offset_ms);
        }
      }
      int navg = 0;
      float avg = driftPpmEstimate(&navg);
      if (o + 96 < reply_size) {
        o += snprintf(reply + o, reply_size - o,
                      "median %+.2f ppm (%.2f s/day) of %d usable samples · rtc=%s\n",
                      (double)avg, (double)avg * 0.0864, navg,
                      AutoDiscoverRTCClock::deviceName());
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
    // public diagnostics responder on the chat identities (see diag_service.h)
    if (strncmp(command, "set ntp ", 8) == 0) {
      strncpy(g_ntp_server, command + 8, sizeof(g_ntp_server) - 1);
      g_ntp_server[sizeof(g_ntp_server) - 1] = 0;
      g_wifi_nvs.begin("multiwifi", false);
      g_wifi_nvs.putString("ntp", g_ntp_server);
      g_wifi_nvs.end();
      g_ntp_started = false;            // restart the client against the new host
      snprintf(reply, reply_size, "OK - ntp server=%s (re-querying now)", g_ntp_server);
      return;
    }
    // Force a measurement now rather than waiting for the schedule. Two of
    // these an hour apart give a real drift sample without sitting through a
    // six-hour interval — the sub-second phase measurement no longer needs a
    // long baseline to beat its own quantisation.
    if (strcmp(command, "clock sync") == 0) {
      if (WiFi.status() != WL_CONNECTED) {
        snprintf(reply, reply_size, "no wifi - cannot reach an NTP server");
        return;
      }
      g_ntp_next_ms = millis();
      snprintf(reply, reply_size, "OK - measuring against %s now (last result: %s)",
               g_ntp_server, g_ntp_last_result);
      return;
    }
    if (strncmp(command, "set ntp.interval ", 17) == 0) {
      uint32_t h = (uint32_t)atoi(command + 17);
      if (h < 1 || h > 168) {
        snprintf(reply, reply_size, "ERR - interval must be 1..168 hours");
        return;
      }
      g_ntp_interval_h = h;
      g_wifi_nvs.begin("multiwifi", false);
      g_wifi_nvs.putUInt("ntpiv", g_ntp_interval_h);
      g_wifi_nvs.end();
      g_ntp_next_ms = millis();      // re-measure now, so the new cadence starts clean
      snprintf(reply, reply_size,
               "OK - ntp interval=%luh (at ~%.0f ppm that is %.2f s of drift between syncs)",
               (unsigned long)g_ntp_interval_h, (double)driftPpmEstimate(),
               (double)driftPpmEstimate() * g_ntp_interval_h * 3600.0 / 1e6);
      return;
    }
    if (strncmp(command, "set bot.webhook ", 16) == 0) {
      const char* url = command + 16;
      if (strcmp(url, "off") == 0 || strcmp(url, "none") == 0) url = "";
      botWebhookSet(url);
      snprintf(reply, reply_size, "OK - bot webhook %s%s", url[0] ? "= " : "disabled", url);
      return;
    }
    if (strcmp(command, "bot test") == 0) {
      snprintf(reply, reply_size, botWebhookTest()
               ? "OK - synthetic message queued to the webhook"
               : "Error - no webhook configured (or the queue is full)");
      return;
    }
    if (strcmp(command, "bot") == 0) {
      char url[128]; botWebhookGet(url, sizeof(url));
      uint32_t sent, failed, dropped; botWebhookStats(&sent, &failed, &dropped);
      char ents[400]; botEntitiesJson(ents, sizeof(ents));
      snprintf(reply, reply_size, "webhook=%s sent=%lu failed=%lu dropped=%lu\n%s",
               url[0] ? url : "(disabled)", (unsigned long)sent, (unsigned long)failed,
               (unsigned long)dropped, ents);
      return;
    }
    if (strncmp(command, "set clock.trim ", 15) == 0) {
      g_trim_enabled = (strncmp(command + 15, "on", 2) == 0);
      g_wifi_nvs.begin("multiwifi", false);
      g_wifi_nvs.putBool("clktrim", g_trim_enabled);
      g_wifi_nvs.end();
      int navg = 0;
      float ppm = driftPpmEstimate(&navg);
      if (!AutoDiscoverRTCClock::hasHardwareRTC()) {
        snprintf(reply, reply_size, "OK - clock.trim=%s, but no hardware RTC was found "
                 "(%s): the clock read here is the one NTP already disciplines, so trimming "
                 "it corrects nothing", g_trim_enabled ? "on" : "off",
                 AutoDiscoverRTCClock::deviceName());
      } else {
        snprintf(reply, reply_size, "OK - clock.trim=%s (rate %.2f ppm from %d samples; "
                 "needs 3 to act)", g_trim_enabled ? "on" : "off", (double)ppm, navg);
      }
      return;
    }
    if (strncmp(command, "set diag ", 9) == 0) {
      bool on = (strncmp(command + 9, "on", 2) == 0);
      diagSetEnabled(on);
      snprintf(reply, reply_size,
               "OK - diagnostics service %s (chat identities answer !ping !time !trace !help)",
               on ? "ON" : "off");
      return;
    }
    if (strcmp(command, "diag") == 0) {
      snprintf(reply, reply_size, "diagnostics service: %s", diagEnabled() ? "on" : "off");
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
    // Typed identity slots. "set slot.2 type room" is the general form;
    // "set slot.chat2 on|off" is kept as the legacy spelling so anything
    // scripted against the old command keeps working.
    if (strncmp(command, "set slot.", 9) == 0 && strncmp(command + 9, "chat", 4) != 0) {
      int idx = atoi(command + 9) - 1;
      const char* arg = strstr(command + 9, " type ");
      SlotType t;
      if (idx < 0 || idx >= MULTI_MAX_CHAT_SLOTS || arg == nullptr ||
          !slotTypeParse(arg + 6, &t)) {
        snprintf(reply, reply_size, "Error - usage: set slot.<1-%d> type off|chat|room",
                 MULTI_MAX_CHAT_SLOTS);
        return;
      }
      SlotType was = slotTypeGet(idx);
      slotTypeSet(idx, t);
      if (was == t) {
        snprintf(reply, reply_size, "OK - slot %d already type=%s", idx + 1, slotTypeName(t));
        return;
      }
      // A live slot cannot change class in place: its mesh object is the wrong
      // class and the stock meshes are not destructible (see the hot-start
      // notes). Turning it off is immediate; coming back as the new type needs
      // the port re-bound at boot.
      g_slot_request[idx] = 2;                      // stop now, from loop()
      snprintf(reply, reply_size,
               "OK - slot %d type %s -> %s. Identity (%s) and storage are UNCHANGED. "
               "%s",
               idx + 1, slotTypeName(was), slotTypeName(t), slotNames()[idx],
               t == SLOT_OFF ? "Stopped."
                             : "Stopped; reboot to start it in the new role.");
      return;
    }
    if (strncmp(command, "set slot.chat", 13) == 0) {
      int idx = atoi(command + 13) - 1;
      const char* arg = strchr(command + 13, ' ');
      if (idx < 0 || idx >= MULTI_MAX_CHAT_SLOTS || arg == nullptr) {
        snprintf(reply, reply_size, "Error - usage: set slot.chat<1-%d> on|off", MULTI_MAX_CHAT_SLOTS);
        return;
      }
      bool on = strcmp(arg + 1, "on") == 0 || strcmp(arg + 1, "1") == 0;
      slotTypeSet(idx, on ? SLOT_CHAT : SLOT_OFF);
      g_slot_request[idx] = on ? 1 : 2;   // applied from loop(), no reboot needed
      snprintf(reply, reply_size, "OK - chat slot %d %s (app port %d)",
               idx + 1, on ? "starting" : "stopping", multiChatSlotPort(idx));
      return;
    }
    // "slot 2 password hunter2" — run a command against a slot's own identity.
    // Rooms already implement password / set name / advert in their stock CLI,
    // so an extra room is managed exactly like the fixed one.
    if (strncmp(command, "slot ", 5) == 0) {
      int idx = atoi(command + 5) - 1;
      const char* sub = strchr(command + 5, ' ');
      if (idx < 0 || idx >= MULTI_MAX_CHAT_SLOTS || sub == nullptr || sub[1] == 0) {
        snprintf(reply, reply_size, "Error - usage: slot <1-%d> <command>", MULTI_MAX_CHAT_SLOTS);
        return;
      }
      IdentityModule* m = slotModule(idx);
      if (m == nullptr) {
        snprintf(reply, reply_size, "Error - slot %d is off", idx + 1);
        return;
      }
      if (!g_slot_started[idx]) {
        snprintf(reply, reply_size, "Error - slot %d (%s) is not running yet",
                 idx + 1, slotTypeName(slotTypeGet(idx)));
        return;
      }
      m->run_command(sub + 1, reply, reply_size);
      return;
    }
    if (strcmp(command, "slots") == 0) {
      size_t o = 0;
      o += snprintf(reply + o, reply_size - o,
                    "fixed: repeater, room, companion (app port %d)\n", 5000);
      for (int i = 0; i < MULTI_MAX_CHAT_SLOTS && o + 80 < reply_size; i++) {
        o += slotDescribe(i, reply + o, reply_size - o);
        o += snprintf(reply + o, reply_size - o, "\n");
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
        const char* why = multiIdImportReason(role, target, pk);
        if (why == nullptr) {
          snprintf(reply, reply_size, "OK - %s identity set (filesystem + mirror); reboot to apply", role);
        } else {
          // include the length we actually received: a truncated or padded
          // command looks identical to a bad key from the caller's side
          snprintf(reply, reply_size, "Error - %s (%s, got %u hex chars)",
                   why, role, (unsigned)strlen(pk));
        }
        return;
      }
    }
    // Read-only listing of the shared partition, biggest first. Nothing else
    // could show what a 94%-full filesystem was actually holding, which is how
    // it silently broke every write on the node.
    if (strcmp(command, "files") == 0) {
      struct Ent { char name[40]; uint32_t size; };
      static const int MAXF = 40;
      static Ent ents[MAXF];   // static: runWebCommand is loop-task only,
      int n = 0; uint32_t total = 0; int extra = 0;   // and this frame is shared
      File root = fs_shared.open("/");
      if (root) {
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
          total += f.size();
          if (n < MAXF) {
            strncpy(ents[n].name, f.path(), sizeof(ents[0].name) - 1);
            ents[n].name[sizeof(ents[0].name) - 1] = 0;
            ents[n].size = f.size();
            n++;
          } else extra++;
        }
      }
      for (int i = 1; i < n; i++) {          // insertion sort, biggest first
        Ent k = ents[i]; int j = i - 1;
        while (j >= 0 && ents[j].size < k.size) { ents[j+1] = ents[j]; j--; }
        ents[j+1] = k;
      }
      uint32_t fs_total = 0, fs_used = 0;
      multiFsStats(&fs_total, &fs_used);
      size_t o = snprintf(reply, reply_size, "%lu files, %lu bytes; fs %lu/%lu used\n",
                          (unsigned long)(n + extra), (unsigned long)total,
                          (unsigned long)fs_used, (unsigned long)fs_total);
      for (int i = 0; i < n && o + 48 < reply_size; i++) {
        o += snprintf(reply + o, reply_size - o, "%7lu %s\n",
                      (unsigned long)ents[i].size, ents[i].name);
      }
      if (extra && o + 24 < reply_size) snprintf(reply + o, reply_size - o, "(+%d more)\n", extra);
      return;
    }
    // Reclaim the per-contact advert blobs left behind by the old uncapped
    // store (see DataStore.cpp). These hold raw advert packets purely so the
    // app can offer "Share contact" — losing them costs nothing but that, and
    // on this node they had eaten 94% of the partition.
    if (strcmp(command, "purge blobs") == 0) {
      const char* dirs[] = { "/comp/bl", "/rep/bl", "/room/bl",
                             "/chat1/bl", "/chat2/bl", "/chat3/bl", "/chat4/bl", "/chat5/bl" };
      uint32_t freed = 0; int removed = 0;
      for (unsigned d = 0; d < sizeof(dirs)/sizeof(dirs[0]); d++) {
        File dir = fs_shared.open(dirs[d]);
        if (!dir) continue;
        // collect first: deleting while iterating the directory is not safe
        static char victims[64][48];   // 3KB — must not sit on the stack
        int nv = 0;
        for (File f = dir.openNextFile(); f && nv < 64; f = dir.openNextFile()) {
          strncpy(victims[nv], f.path(), sizeof(victims[0]) - 1);
          victims[nv][sizeof(victims[0]) - 1] = 0;
          freed += f.size();
          nv++;
        }
        dir.close();
        for (int i = 0; i < nv; i++) if (fs_shared.remove(victims[i])) removed++;
      }
      uint32_t t = 0, u = 0; multiFsStats(&t, &u);
      snprintf(reply, reply_size,
               "removed %d blob files (%lu bytes of content); fs now %lu/%lu used",
               removed, (unsigned long)freed, (unsigned long)u, (unsigned long)t);
      return;
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
  int drift_n = 0;
  float ppm_avg = driftPpmEstimate(&drift_n);

  int n = snprintf(out, cap,
    "{\"enabled\":%s,\"powered\":%s,\"lock\":%s,\"sats\":%ld,\"switch_on\":%s,\"rx_bytes\":%lu,"
    "\"every_h\":%lu,\"next_s\":%lu,\"last_sync\":%lu,\"syncs\":%lu,"
    "\"skips_low_batt\":%lu,\"drift_ppm\":%.2f,\"searching_s\":%lu,\"state\":\"%s\","
    "\"clock_source\":\"%s\",\"epoch\":%lu,"
    "\"synced_ago_s\":%lu,\"ntp_server\":\"%s\",\"ntp_syncs\":%lu,\"ntp_state\":\"%s\","
    "\"rtc\":\"%s\",\"rtc_hw\":%s,\"drift_ppm_est\":%.2f,\"drift_samples\":%d,"
    "\"trim\":%s,\"trim_steps\":%lu,\"trim_pending_ms\":%ld,\"subsec\":%s,\"drift_gap_ms\":%u,\"osc_stopped\":%d",
    g_gps_sync_hours > 0 ? "true" : "false",
    powered ? "true" : "false",
    valid ? "true" : "false",
    sats,
    gpsSwitchOn() ? "true" : "false",
    (unsigned long)(gps ? gps->rawBytesRx() : 0),
    (unsigned long)g_gps_sync_hours, (unsigned long)next_s,
    (unsigned long)g_gps_last_sync_epoch, (unsigned long)g_drift_count,
    (unsigned long)g_gps_skips_low_batt, (double)ppm,
    (unsigned long)(g_gps_search_start_ms ? (now - g_gps_search_start_ms) / 1000 : 0),
    g_gps_last_result,
    g_clock_src,
    (unsigned long)rtc_clock.getCurrentTime(),
    (unsigned long)multiClockSyncedAgo(), g_ntp_server,
    (unsigned long)g_ntp_syncs, g_ntp_last_result,
    AutoDiscoverRTCClock::deviceName(),
    AutoDiscoverRTCClock::hasHardwareRTC() ? "true" : "false",
    (double)ppm_avg, drift_n,
    g_trim_enabled ? "true" : "false", (unsigned long)g_trim_steps,
    (long)g_trim_applied_ms, g_rtcp_done ? "true" : "false",
    (unsigned)g_rtcp_gap_ms, g_rtc_osc_stopped);

  if (valid && n > 0 && (size_t)n < cap) {
    n += snprintf(out + n, cap - n, ",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%ld",
                  gps->getLatitude() / 1e6, gps->getLongitude() / 1e6, gps->getAltitude());
  }
  if (n > 0 && (size_t)n < cap) n += snprintf(out + n, cap - n, "}");
  return n;
}

// Usage of the ONE shared SPIFFS partition. The composition mounts its own
// fs::SPIFFSFS instance (label "fs"), not the global SPIFFS singleton, so
// anything asking the singleton gets zeroes and learns nothing.
void multiFsStats(uint32_t* total, uint32_t* used) {
  if (total) *total = (uint32_t)fs_shared.totalBytes();
  if (used)  *used  = (uint32_t)fs_shared.usedBytes();
}

// Where the clock last came from, for anything that needs to say so out loud.
const char* multiClockSource() { return g_clock_src; }
// Seconds since that sync; 0 means it has never been disciplined at all.
uint32_t multiClockSyncedAgo() {
  if (g_clock_last_ms == 0) return 0;
  return (uint32_t)((millis() - g_clock_last_ms) / 1000);
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
    // Resolve by TYPE, not by assuming chat: a slot configured as a room must
    // start its room instance, not a companion on the same port.
    IdentityModule* m = slotModule(i);
    if (m == nullptr) { g_core->setPortActive(g_slot_port_idx[i], false); continue; }
    if (req == 1) {
      if (!g_slot_started[i]) {
        fs_chat[i].begin(slotFsDir(i));
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
  // Give the observer our time source so it can difference peers' advert
  // timestamps against it. Ours is the disciplined one here — NTP every 6h
  // while WiFi is up, GPS as the fallback — so the delta it records is
  // essentially the other node's error, not a difference of two unknowns.
  g_core->observer().setClock(&rtc_clock);
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
  // per-packet coding rate for the trace: the sender's, out of the LoRa header
  // of the frame the modem has just decoded
  g_core->setRxCodingRateFn([]() -> uint8_t { return radio_driver.getLastRxCodingRate(); });
  g_core->setRxAirtimeFn([](int len, uint8_t cr) -> uint32_t {
    return radio_driver.getEstAirtimeForCR(len, cr);
  });
  g_core->setCodingRate(g_radio.cr);
  g_core->setPortName(g_core->addPort(port_rep),  "repeater");
  g_core->setPortName(g_core->addPort(port_room), "room");
  g_core->setPortName(g_core->addPort(port_comp), "companion");

  // Optional chat identity slots. Every slot's port is registered up front —
  // inactive unless enabled — so port indices never move and a slot can be
  // switched on or off later without a reboot.
  multiChatSlotsInit();
  roomInstancesInit(slotNames());   // room instances 1..N back typed slots
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    IdentityModule* m = slotModule(i);
    g_slot_port_idx[i] = g_core->addPort(port_chat[i], m != nullptr);
    // Name the port from the slot's IDENTITY name, not from the module: an
    // off slot has no module, and this line used to be safe only because the
    // old lookup returned one unconditionally. A port keeps its name whatever
    // the slot is (or is not) running, which is what the trace wants anyway.
    g_core->setPortName(g_slot_port_idx[i], slotNames()[i]);
    if (m != nullptr) {
      fs_chat[i].begin(slotFsDir(i));
      g_modules[NUM_MODULES++] = m;
    }
  }

  WebPanelServer::setExtRoutesRegistrar(&multiWebRegisterRoutes);   // unified panel + APIs
  WebPanelServer::setExtOwnsIndex(true);                            // panel served at "/" (stock SPA stays at /app)

  repeater_module.setup(&fs_rep, &port_rep);
  room_module.setup(&fs_room, &port_room);
  companion_module.setup(&fs_comp, &port_comp);
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    { IdentityModule* m = slotModule(i);
      if (m) { m->setup(&fs_chat[i], &port_chat[i]); g_slot_started[i] = true; } }
  }

  // tell the arbiter each identity's key so it can spot our own hash coming
  // back in another node's relayed path (see setPortIdentity)
  { uint8_t pk[32];
    repeater_module.get_pubkey(pk);  g_core->setPortIdentity(0, pk);
    room_module.get_pubkey(pk);      g_core->setPortIdentity(1, pk);
    companion_module.get_pubkey(pk); g_core->setPortIdentity(2, pk);
    for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
      IdentityModule* m = slotModule(i);
      if (m == nullptr) continue;
      m->get_pubkey(pk);
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
  { String n = g_wifi_nvs.getString("ntp", "");
    if (n.length() > 0) { strncpy(g_ntp_server, n.c_str(), sizeof(g_ntp_server) - 1);
                          g_ntp_server[sizeof(g_ntp_server) - 1] = 0; } }
  g_trim_enabled = g_wifi_nvs.getBool("clktrim", true);
  g_ntp_interval_h = g_wifi_nvs.getUInt("ntpiv", 6);
  g_wifi_nvs.end();
  driftInit();

  // Sample the oscillator-stop flag ONCE, here, before the first NTP sync gets
  // a chance to write the chip and clear it. This is the standing question
  // about this node: every OTA it comes back tens of minutes out (25 and 28
  // minutes on the last two), which is far more than a 24 ppm crystal can
  // explain and has to mean the timekeeping was interrupted rather than drifted.
  // If this reads "stopped" after a reflash, that is the answer.
  g_rtc_osc_stopped = AutoDiscoverRTCClock::oscillatorStopped();
  Serial.printf("[clock] rtc=%s oscillator-stopped-since-last-set=%s\n",
                AutoDiscoverRTCClock::deviceName(),
                g_rtc_osc_stopped < 0 ? "unknown (chip cannot report)"
                                      : (g_rtc_osc_stopped ? "YES" : "no"));

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
  gpsSyncTick();                  // scheduled GPS clock discipline (fallback)
  ntpSyncTick();                  // ...and NTP, which is primary when wifi is up
  clockTrimTick();                // ...and hold the RTC steady between the two
  botTick();                      // push newly-arrived messages to the webhook
#ifdef WITH_MQTT_UPLINK
  mqttTick();                     // MQTT uplink (JWT-signed by our own identity)
#endif
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
