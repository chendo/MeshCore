#pragma once

#include <stddef.h>

#if defined(ESP_PLATFORM)
  #include <WiFi.h>
  #if defined(WITH_MQTT_UPLINK) && !defined(WITH_WEB_PANEL)
    #define WITH_WEB_PANEL 1
  #endif
  #if WITH_WEB_PANEL
    #include <esp_https_server.h>
    #include <esp_http_server.h>
  #endif
#endif

class WebPanelCommandRunner {
public:
  virtual ~WebPanelCommandRunner() = default;
  virtual void runWebCommand(const char* command, char* reply, size_t reply_size) = 0;
  virtual const char* getWebAdminPassword() const = 0;
  virtual bool isWebStatsEnabled() const { return false; }
  virtual bool formatWebStatsSummaryJson(char* reply, size_t reply_size) {
    if (reply != nullptr && reply_size > 0) {
      reply[0] = 0;
    }
    return false;
  }
  virtual bool formatWebStatsSeriesJson(const char* series, char* reply, size_t reply_size) {
    (void)series;
    if (reply != nullptr && reply_size > 0) {
      reply[0] = 0;
    }
    return false;
  }
};

class WebPanelServer {
public:
  WebPanelServer();

#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  // Extension hook: a target/composition can register extra httpd routes on the
  // panel's server. The registrar runs at the end of every successful start().
  typedef void (*ExtRoutesFn)(httpd_handle_t server, WebPanelServer* panel);
  static void setExtRoutesRegistrar(ExtRoutesFn fn) { _ext_routes_fn = fn; }
  // when set, start() does not register the "/" route — the extension
  // registrar serves its own index page (stock SPA stays at /app)
  static void setExtOwnsIndex(bool owns) { _ext_owns_index = owns; }
  // for extension handlers: session-token check + activity refresh
  bool extAuthorize(httpd_req_t* req);
#endif

  void setCommandRunner(WebPanelCommandRunner* runner);
  bool start();
  void stop(bool clear_session = true);
  void stopRedirectServer();
  bool isRunning() const;
  bool hasSessionToken() const;
  bool shouldAutoLock(unsigned long now_ms) const;
  void lockSession();

private:
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  struct RouteContext {
    WebPanelServer* self;
  };

  WebPanelCommandRunner* _runner;
  httpd_handle_t _server;
  httpd_handle_t _redirect_server;
  // Concurrent sessions: several browsers/devices/scripts can hold their own
  // token at once, and the table is persisted so a reboot (e.g. an OTA
  // update) doesn't log everyone out.
  static const int MAX_SESSIONS = 4;
  char _token[33];                       // most recently issued (compat)
  char _tokens[MAX_SESSIONS][33];
  // When the table is full the LEAST RECENTLY USED session is evicted. "Used"
  // means a request actually authenticated with it, not when it was issued —
  // a browser left open on the panel keeps its slot, while a token minted by a
  // one-off curl and never seen again is the first to go. Ordering is a
  // monotonic counter rather than a clock: these survive reboots in RTC memory,
  // and millis() restarts at zero, which would scramble the ordering.
  mutable uint32_t _tok_used[MAX_SESSIONS];   // _use_seq value at that slot's last use
  mutable uint32_t _use_seq;                  // bumped on every successful auth
  void loadSessions();
  void saveSessions();
  bool addSession(const char* tok);
  void touchSession(int slot) const;     // called from the const auth path
  unsigned long _last_activity_ms;
  RouteContext _route_context;

  static esp_err_t handleIndex(httpd_req_t* req);
  static esp_err_t handleFavicon(httpd_req_t* req);
  static esp_err_t handleHttpRedirect(httpd_req_t* req);
  static esp_err_t handleApp(httpd_req_t* req);
  static esp_err_t handleStatsPage(httpd_req_t* req);
  static esp_err_t handleLogin(httpd_req_t* req);
  static esp_err_t handleSession(httpd_req_t* req);
  static esp_err_t handleCommand(httpd_req_t* req);
  static esp_err_t handleFirmwareUpdate(httpd_req_t* req);
  static esp_err_t handleStats(httpd_req_t* req);

  bool readRequestBody(httpd_req_t* req, char* buffer, size_t buffer_size) const;
  void refreshToken();
  bool isAuthorized(httpd_req_t* req) const;
  void noteActivity();
  static ExtRoutesFn _ext_routes_fn;
  static bool _ext_owns_index;
#else
  WebPanelCommandRunner* _runner;
#endif
};
