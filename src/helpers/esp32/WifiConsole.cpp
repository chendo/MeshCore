#include "WifiConsole.h"
#include <time.h>

/* Credentials live in their own NVS namespace. Deliberately not in NodePrefs:
   that struct is written whole on every savePrefs(), it is shared with the
   nRF52 builds that have no WiFi, and appending to it churns the prefs file
   for a field three quarters of the fleet cannot use. */
static const char* NVS_NS = "hydra-wifi";

void WifiConsole::load() {
  Preferences p;
  if (!p.begin(NVS_NS, /*readOnly=*/true)) return;   // never written yet
  p.getString("ssid", _ssid, sizeof(_ssid));
  p.getString("pwd", _pwd, sizeof(_pwd));
  p.end();
}

void WifiConsole::save() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putString("ssid", _ssid);
  p.putString("pwd", _pwd);
  p.end();
}

void WifiConsole::associate() {
  WiFi.persistent(false);        // don't wear NVS with a rewrite every boot
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(_ssid, _pwd);       // returns immediately; loop() watches status
  _next_retry = millis() + WIFI_RETRY_MS;
}

void WifiConsole::begin() {
  load();
  if (!isConfigured()) return;   // radio present, nothing configured: stay idle
  associate();
}

void WifiConsole::setCredentials(const char* ssid, const char* pwd) {
  strncpy(_ssid, ssid ? ssid : "", sizeof(_ssid) - 1); _ssid[sizeof(_ssid)-1] = 0;
  strncpy(_pwd, pwd ? pwd : "", sizeof(_pwd) - 1);     _pwd[sizeof(_pwd)-1] = 0;
  save();
  if (!isConfigured()) {         // empty ssid means "forget WiFi"
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _was_up = false;
    return;
  }
  WiFi.disconnect();
  associate();
}

void WifiConsole::status(char* out, size_t out_sz) const {
  if (!isConfigured()) { snprintf(out, out_sz, "wifi: not configured"); return; }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(out, out_sz, "wifi: %s - joining", _ssid);
    return;
  }
  IPAddress a = WiFi.localIP();
  snprintf(out, out_sz, "wifi: %s %d.%d.%d.%d:%d rssi %d%s",
           _ssid, a[0], a[1], a[2], a[3], _port, (int)WiFi.RSSI(),
           _ntp_done ? " ntp ok" : "");
}

void WifiConsole::onLinkUp() {
  if (!_started) { _server.begin(); _server.setNoDelay(true); _started = true; }
  if (_ntp_on && !_ntp_done) configTime(0, 0, _ntp[0], _ntp[1], _ntp[2]);
}

void WifiConsole::pollNtp() {
  if (!_ntp_on || _ntp_done) return;
  time_t now = time(nullptr);
  if (now > 1700000000) {        // plausible epoch => SNTP has landed
    _ntp_epoch = (uint32_t)now;
    _ntp_done = true;
  }
}

void WifiConsole::loop() {
  if (!isConfigured()) return;   // no credentials: the radio stays off
  bool up = isConnected();

  if (!up) {
    // Association dropped or never made. Retry on a timer rather than
    // hammering; WiFi.begin() while already associating just restarts it.
    if ((long)(millis() - _next_retry) >= 0) {
      WiFi.disconnect();
      associate();
    }
    _was_up = false;
    return;
  }

  /* Push the retry deadline out while the link is good. Without this it stays
     wherever associate() left it, goes stale, and then the first transient
     non-WL_CONNECTED reading -- a missed beacon is enough -- is already past
     the deadline, so we tear down a link that was about to recover. The window
     must mean "this long with no link", not "this long since we called
     begin()". */
  _next_retry = millis() + WIFI_RETRY_MS;

  if (!_was_up) { _was_up = true; onLinkUp(); }
  pollNtp();

  if (!_started) return;

  // One client at a time. A second connection replaces the first, so a stale
  // half-open socket can't lock the console out for ever.
  if (_server.hasClient()) {
    WiFiClient incoming = _server.available();
    if (_client && _client.connected()) _client.stop();
    _client = incoming;
    _client.setNoDelay(true);
    _len = 0; _line[0] = 0; _ready = false;
  }

  if (!_client || !_client.connected()) return;

  while (_client.available() && _len < (int)sizeof(_line) - 1) {
    char c = _client.read();
    if (c == '\n') continue;
    if (c == '\r') { _line[_len] = 0; _ready = true; break; }
    _line[_len++] = c;
    _line[_len] = 0;
  }
  if (_len == (int)sizeof(_line) - 1) _ready = true;   // overlong line: take it
}

const char* WifiConsole::takeLine() {
  if (!_ready) return nullptr;
  _ready = false;
  _len = 0;
  return _line;
}

void WifiConsole::reply(const char* text) {
  if (_client && _client.connected()) {
    _client.print("  -> ");
    _client.println(text);
  }
}
