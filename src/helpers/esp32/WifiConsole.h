#pragma once

/* WiFi station + a line-oriented TCP console, for ESP32 nodes that want to be
 * reachable without a serial cable.
 *
 * Two things matter here.
 *
 * 1. Nothing blocks. begin() only starts the association; the loop polls it.
 *    A node with LOOP_WATCHDOG_MS set will reset itself if a join is allowed to
 *    spin, and a repeater on a mast must come up whether the AP answers or not.
 *
 * 2. The TCP console is NOT the serial console. CommonCLI treats
 *    sender_timestamp == 0 as "typed at the physically attached cable" and only
 *    then will it export a private key. A socket on the LAN has not earned
 *    that, so the caller is expected to pass a non-zero timestamp for TCP
 *    input. See WIFI_CONSOLE_SENDER below.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#ifndef WIFI_CONSOLE_PORT
  #define WIFI_CONSOLE_PORT 4403
#endif
#ifndef WIFI_RETRY_MS
  #define WIFI_RETRY_MS 30000
#endif

class WifiConsole {
public:
  WifiConsole(int port = WIFI_CONSOLE_PORT)
    : _port(port), _server(port),
      _started(false), _was_up(false), _next_retry(0), _len(0) {
    _line[0] = 0; _ssid[0] = 0; _pwd[0] = 0;
  }

  /** Load credentials from NVS and, if an SSID is set, start associating.
   *  Returns immediately either way. Safe to call with nothing configured. */
  void begin();

  /** Set and persist credentials, then (re)associate. An empty ssid turns
   *  WiFi off and forgets it. */
  void setCredentials(const char* ssid, const char* pwd);

  bool isConfigured() const { return _ssid[0] != 0; }
  const char* ssid() const { return _ssid; }

  /** One-line status for the CLI. */
  void status(char* out, size_t out_sz) const;

  /** Poll association state and service the socket. Call every loop. */
  void loop();

  bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
  IPAddress ip() const { return WiFi.localIP(); }

  /** A complete line arrived from the TCP client, or nullptr. Caller-owned
   *  until the next loop(). */
  const char* takeLine();

  /** Write a reply back to the TCP client, if one is attached. */
  void reply(const char* text);

  /** One-shot SNTP sync once the link is up; safe to leave enabled. */
  void enableNtp(const char* s1, const char* s2 = nullptr, const char* s3 = nullptr) {
    _ntp[0] = s1; _ntp[1] = s2; _ntp[2] = s3; _ntp_on = true;
  }
  bool ntpSynced() const { return _ntp_done; }
  uint32_t ntpEpoch() const { return _ntp_epoch; }

private:
  char _ssid[33];      // 32 + NUL, the 802.11 maximum
  char _pwd[65];       // 64 + NUL, a WPA2 passphrase/PSK
  int _port;
  WiFiServer _server;
  WiFiClient _client;
  bool _started, _was_up;
  unsigned long _next_retry;

  char _line[160];
  int _len;
  bool _ready = false;

  const char* _ntp[3] = { nullptr, nullptr, nullptr };
  bool _ntp_on = false, _ntp_done = false;
  uint32_t _ntp_epoch = 0;

  void onLinkUp();
  void pollNtp();
  void load();
  void save();
  void associate();
};
