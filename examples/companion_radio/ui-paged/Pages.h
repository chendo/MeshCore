#pragma once

#include <helpers/ui/PagedScreen.h>
#include <helpers/ui/NeighboursScreen.h>
#include <helpers/PowerMonitor.h>
#include "RecentMessages.h"
#include "../NodePrefs.h"

/* Shared helpers. Dividers are drawn as TEXT everywhere in this tree: in
   GxEPDDisplay, setCursor() adds EINK_Y_OFFSET and a font baseline correction
   while fillRect() adds neither, so a rule placed against a text baseline lands
   in a different coordinate space and strikes through the text. */
inline void uiRule(DisplayDriver& d, int y) {
  char buf[48];
  int w = d.getTextWidth("-");
  int n = w > 0 ? d.width() / w : 0;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  for (int i = 0; i < n; i++) buf[i] = '-';
  buf[n > 0 ? n : 0] = 0;
  d.setColor(UIColor::secondary_txt);
  d.drawTextLeftAlign(0, y, buf);
  d.setColor(UIColor::primary_txt);
}

/* The action footer. Drawn by each page at the bottom of its own body, in the
   secondary colour, so what the buttons do is always on screen rather than
   remembered. */
inline void uiActions(DisplayDriver& d, int y, int avail_h, int pitch,
                      const char* tap, const char* hold) {
  char buf[40];
  d.setColor(UIColor::secondary_txt);
  if (tap != NULL && y + pitch <= avail_h) {
    snprintf(buf, sizeof(buf), "tap: %s", tap);
    d.drawTextLeftAlign(0, y, buf);
    y += pitch;
  }
  if (hold != NULL && y + pitch <= avail_h) {
    snprintf(buf, sizeof(buf), "hold: %s", hold);
    d.drawTextLeftAlign(0, y, buf);
  }
  d.setColor(UIColor::primary_txt);
}

inline void uiRow(DisplayDriver& d, int y, const char* l, const char* r) {
  if (l) d.drawTextLeftAlign(0, y, l);
  if (r) d.drawTextRightAlign(d.width(), y, r);
}

/* The state the pages share with the task. Passed by reference so a page never
   reaches for a global, which is what makes these testable on the host. */
struct UIContext {
  NodePrefs*      prefs;
  RecentMessages* recent;
  PowerMonitor*   power;
  int             msg_count;
  bool            bt_enabled;
  bool            connected;
  bool            buzzer_muted;
  bool            gps_on;
  bool            gps_present;
  const char*     node_name;
};

// ---------------------------------------------------------------- home

class HomePage : public UIPage {
  UIContext& _c;
  int _pitch;
public:
  HomePage(UIContext& c, int pitch) : _c(c), _pitch(pitch) { }
  const char* tapLabel() const override { return _c.buzzer_muted ? "unmute" : "mute"; }

  int renderBody(DisplayDriver& d, int avail_h) override {
    char l[40], r[24];
    int y = 2;
    d.setColor(UIColor::primary_txt);
    d.translateUTF8ToBlocks(l, _c.node_name ? _c.node_name : "(unnamed)", sizeof(l));
    d.drawTextEllipsized(0, y, d.width(), l);
    y += _pitch;
    uiRule(d, y); y += _pitch;

    snprintf(r, sizeof(r), "%d", _c.msg_count);
    uiRow(d, y, "msgs", r); y += _pitch;

    uiRow(d, y, "bt", _c.bt_enabled ? (_c.connected ? "linked" : "on") : "off");
    y += _pitch;

    int pct = _c.power ? _c.power->percent() : -1;
    if (pct < 0) snprintf(r, sizeof(r), "n/a");
    else if (_c.power->isCharging()) snprintf(r, sizeof(r), "%d%% chg", pct);
    else snprintf(r, sizeof(r), "%d%%", pct);
    uiRow(d, y, "batt", r); y += _pitch;

    if (y + _pitch <= avail_h) {
      uiRow(d, y, "buzzer", _c.buzzer_muted ? "muted" : "on");
      y += _pitch;
    }
    uiActions(d, y, avail_h, _pitch, tapLabel(), holdLabel());
    return 5000;
  }
};

// ------------------------------------------------------------ messages

class MessagesPage : public UIPage {
  UIContext& _c;
  int _pitch;
public:
  MessagesPage(UIContext& c, int pitch) : _c(c), _pitch(pitch) { }

  int renderBody(DisplayDriver& d, int avail_h) override {
    char l[72];
    int y = 2;
    d.setColor(UIColor::primary_txt);
    snprintf(l, sizeof(l), "MESSAGES");
    d.drawTextLeftAlign(0, y, l);
    snprintf(l, sizeof(l), "%d", _c.msg_count);
    d.drawTextRightAlign(d.width(), y, l);
    y += _pitch;
    uiRule(d, y); y += _pitch;

    if (_c.recent == NULL || _c.recent->count() == 0) {
      d.setColor(UIColor::secondary_txt);
      d.drawTextCentered(d.width() / 2, y + _pitch, "no messages yet");
      return 10000;
    }

    /* Two lines per message: who, then what. A single line would truncate the
       text to nothing once the name has taken its share. */
    for (int i = 0; i < _c.recent->count() && y + _pitch * 2 <= avail_h; i++) {
      const RecentMessages::Entry* e = _c.recent->at(i);
      if (e == NULL) break;
      d.setColor(UIColor::primary_txt);
      d.translateUTF8ToBlocks(l, e->from, sizeof(l));
      d.drawTextEllipsized(0, y, d.width(), l);
      y += _pitch;
      d.setColor(UIColor::secondary_txt);
      d.translateUTF8ToBlocks(l, e->text, sizeof(l));
      d.drawTextEllipsized(0, y, d.width(), l);
      y += _pitch;
    }
    d.setColor(UIColor::primary_txt);
    return 10000;
  }
};

// --------------------------------------------------------------- radio

class RadioPage : public UIPage {
  UIContext& _c;
  int _pitch;
public:
  RadioPage(UIContext& c, int pitch) : _c(c), _pitch(pitch) { }

  int renderBody(DisplayDriver& d, int avail_h) override {
    char l[32], r[24];
    int y = 2;
    d.setColor(UIColor::primary_txt);
    d.drawTextLeftAlign(0, y, "RADIO");
    d.drawTextRightAlign(d.width(), y, "read-only");
    y += _pitch;
    uiRule(d, y); y += _pitch;

    NodePrefs* p = _c.prefs;
    if (p == NULL) return 30000;

    /* Integer formatting: %f would pull printf's float support into the image
       for the sake of four numbers. */
    uint32_t khz = (uint32_t)(p->freq * 1000.0f + 0.5f);
    snprintf(r, sizeof(r), "%u.%03u", (unsigned)(khz / 1000), (unsigned)(khz % 1000));
    uiRow(d, y, "freq", r); y += _pitch;

    snprintf(r, sizeof(r), "%u", (unsigned)(p->bw + 0.5f));
    uiRow(d, y, "bw", r); y += _pitch;

    snprintf(l, sizeof(l), "sf %u", (unsigned)p->sf);
    snprintf(r, sizeof(r), "cr %u", (unsigned)p->cr);
    uiRow(d, y, l, r); y += _pitch;

    if (y + _pitch <= avail_h) {
      snprintf(r, sizeof(r), "%ddBm", (int)p->tx_power_dbm);
      uiRow(d, y, "power", r);
    }
    // The app owns these; nothing here changes on its own.
    return 30000;
  }
};

/* ACTION PAGES.
   The M1 has exactly one usable button: pin 42. PIN_BUTTON2 produced no edge in
   a full press test as either INPUT or INPUT_PULLUP, and PIN_GPS_SWITCH is a
   hardware kill line for the GPS module, not a spare input -- upstream only
   reads it to report "gps off(hw)".
   So a global action button does not exist, and actions live on pages instead,
   each one saying what it does. That is how ui-new works on this board too. */
class ActionPage : public UIPage {
  const char* _title;
  const char* _tap;
  const char* _hold;
  int _pitch;
  const char** _status;      // may be NULL; a live value shown under the title
public:
  ActionPage(const char* title, const char* tap, const char* hold, int pitch,
             const char** status = NULL)
    : _title(title), _tap(tap), _hold(hold), _pitch(pitch), _status(status) { }
  const char* tapLabel() const override { return _tap; }
  const char* holdLabel() const override { return _hold; }

  int renderBody(DisplayDriver& d, int avail_h) override {
    int y = 2;
    d.setColor(UIColor::primary_txt);
    d.drawTextLeftAlign(0, y, _title);
    y += _pitch;
    uiRule(d, y); y += _pitch;
    if (_status != NULL && *_status != NULL) {
      d.drawTextCentered(d.width() / 2, y + _pitch, *_status);
    }
    uiActions(d, y + _pitch * 3, avail_h, _pitch, _tap, _hold);
    return 5000;
  }
};

// ----------------------------------------------------------------- gps

#if ENV_INCLUDE_GPS == 1
#include <helpers/sensors/LocationProvider.h>

class GpsPage : public UIPage {
  UIContext& _c;
  int _pitch;
  LocationProvider* _loc;
  PagedScreen* _owner;
public:
  GpsPage(UIContext& c, int pitch, PagedScreen* owner)
    : _c(c), _pitch(pitch), _loc(NULL), _owner(owner) { }
  void setProvider(LocationProvider* l) { _loc = l; }
  const char* tapLabel() const override { return _c.gps_on ? "gps off" : "gps on"; }

  int renderBody(DisplayDriver& d, int avail_h) override {
    char r[28];
    int y = 2;
    d.setColor(UIColor::primary_txt);
    d.drawTextLeftAlign(0, y, "GPS");
    d.drawTextRightAlign(d.width(), y, _c.gps_on ? "sw on" : "sw off");
    y += _pitch;
    uiRule(d, y); y += _pitch;

    /* Two things can turn the GPS off and they are worth telling apart: the
       slide switch cuts the module's power, the pref only stops us asking it.
       A page that showed one "off" would send you hunting for the wrong one. */
    bool hw_on = digitalRead(PIN_GPS_SWITCH);
    if (!hw_on) {
      d.setColor(UIColor::warning_txt);
      d.drawTextLeftAlign(0, y, "switch: OFF");
      d.setColor(UIColor::primary_txt);
      y += _pitch;
    }
    if (!_c.gps_on || !hw_on || _loc == NULL) {
      d.setColor(UIColor::secondary_txt);
      d.drawTextLeftAlign(0, y, !hw_on ? "hardware off"
                              : (_c.gps_on ? "no receiver" : "software off"));
      d.setColor(UIColor::primary_txt);
      y += _pitch;
    } else {
      snprintf(r, sizeof(r), "%ld", _loc->satellitesCount());
      uiRow(d, y, "sats", r); y += _pitch;
      uiRow(d, y, "fix", _loc->isValid() ? "yes" : "searching"); y += _pitch;
      if (_loc->isValid() && y + _pitch * 2 <= avail_h) {
        /* Degrees are stored scaled by 1e6; printed as integer parts so the
           page needs no float formatting. */
        long la = _loc->getLatitude(), lo = _loc->getLongitude();
        snprintf(r, sizeof(r), "%ld.%04ld", la / 1000000, labs(la % 1000000) / 100);
        uiRow(d, y, "lat", r); y += _pitch;
        snprintf(r, sizeof(r), "%ld.%04ld", lo / 1000000, labs(lo % 1000000) / 100);
        uiRow(d, y, "lon", r); y += _pitch;
      }
    }
    uiActions(d, y, avail_h, _pitch, tapLabel(), NULL);
    return _c.gps_on ? 5000 : 30000;
  }

  // Handled by the task, which owns the SensorManager; see UITask::pageAction.
  bool handleInput(char c) override { return false; }
};
#endif

// ------------------------------------------------------------ shutdown

/* Power-off needs no arming step. Under the one rule this UI follows -- tap is
   the light action, hold is the heavy one -- a hold IS the deliberate act, and a
   button that has to be held for a second will not be triggered by a pocket. An
   earlier version made it two double-taps, which was ceremony that the rule
   already provides. */
class ShutdownPage : public UIPage {
  UIContext& _c;
  int _pitch;
public:
  ShutdownPage(UIContext& c, int pitch) : _c(c), _pitch(pitch) { }
  const char* holdLabel() const override { return "power off"; }

  int renderBody(DisplayDriver& d, int avail_h) override {
    int y = 2;
    d.setColor(UIColor::primary_txt);
    d.drawTextLeftAlign(0, y, "POWER OFF");
    y += _pitch;
    uiRule(d, y); y += _pitch * 2;
    d.setColor(UIColor::secondary_txt);
    d.drawTextCentered(d.width() / 2, y, "hold B2 to shut down");
    d.setColor(UIColor::primary_txt);
    uiActions(d, y + _pitch * 2, avail_h, _pitch, NULL, holdLabel());
    return 30000;
  }
};
