#include "UITask.h"
#include "../MyMesh.h"
#include "target.h"

/* Pitch and text size. 0 selects GxEPDDisplay's compact 5x7 font, which turns
   the 200x200 panel from 20 columns by 9 lines into 33 by 25 -- the difference
   between a headline and a table. See patches/gxepd-compact-text-size. */
#define UI_PITCH      6
#define UI_TEXT_SIZE  0

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS 0
#endif

extern MyMesh the_mesh;

UITask::UITask(mesh::MainBoard* board, MultiSerialInterface* serial)
  : AbstractUITask(board, serial),
    _display(NULL), _sensors(NULL), _node_prefs(NULL),
    _neighbours(rfObserver()),
    _screen(UI_PITCH, UI_TEXT_SIZE),
    _home(_ctx, UI_PITCH),
    _messages(_ctx, UI_PITCH),
    _neigh(_neighbours, NULL, NULL, UI_PITCH, 5000, UI_TEXT_SIZE),
    _radio(_ctx, UI_PITCH),
    /* Same configuration as the button that works: plain INPUT, active low,
       trusting the board's external pull-up. */
    _btn2(UI_BUTTON2_PIN, 700, true, false),
    _advert("ADVERT", "zero-hop", "flood", UI_PITCH),
    _bluetooth("BLUETOOTH", "toggle", NULL, UI_PITCH, &_bt_status),
    _bt_status("?"),
#if ENV_INCLUDE_GPS == 1
    _gps(_ctx, UI_PITCH, &_screen),
#endif
    _shutdown(_ctx, UI_PITCH),
    _msgcount(0), _next_render(0), _auto_off(0), _ok(false),
    _idx_home(0), _idx_gps(-1), _idx_shutdown(-1), _idx_advert(-1), _idx_bt(-1) {
  memset(&_ctx, 0, sizeof(_ctx));
  _radio_sub[0] = 0;
}

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _node_prefs = node_prefs;

  _ctx.prefs = node_prefs;
  _ctx.recent = &_recent;
  _ctx.power = &_power;
  _ctx.node_name = node_prefs ? node_prefs->node_name : "";
  _ctx.gps_present = false;

  _power.begin(PowerMonitor::DEFAULT_MIN_MV, PowerMonitor::DEFAULT_MAX_MV);

  /* The observer must know our own key, or it records us as our own peer every
     time one of our floods comes back through a repeater. */
  rfObserver().addSelfKey(the_mesh.self_id.pub_key);
  rfObserver().setClock(&rtc_clock);

  _neigh.setTitle(_ctx.node_name);
  _neigh.setSubtitle(NULL);

  _idx_home = _screen.numPages(); _screen.addPage(&_home);
  _screen.addPage(&_messages);
  _screen.addPage(&_neigh);
  _screen.addPage(&_radio);
  _idx_advert = _screen.numPages(); _screen.addPage(&_advert);
  _idx_bt     = _screen.numPages(); _screen.addPage(&_bluetooth);
#if ENV_INCLUDE_GPS == 1
  if (_sensors != NULL) {
    _gps.setProvider(_sensors->getLocationProvider());
    _ctx.gps_present = true;
    _idx_gps = _screen.numPages();
    _screen.addPage(&_gps);
  }
#endif
  _idx_shutdown = _screen.numPages(); _screen.addPage(&_shutdown);

  user_btn.begin();      // the variant's, configured the way this board needs
  _btn2.begin();

  /* main.cpp has already called display.begin() and passes NULL when it
     failed, so this must not begin() again. It must still turnOn(): begin()
     leaves isOn() false, and isOn() gates every draw, so without it the panel
     stays blank for ever and nothing reports an error. */
  _ok = (_display != NULL);
  if (_ok) _display->turnOn();

#ifdef PIN_BUZZER
  buzzer.begin();
  if (_node_prefs) buzzer.quiet(_node_prefs->buzzer_quiet != 0);
#endif
#if AUTO_OFF_MILLIS > 0
  _auto_off = millis() + AUTO_OFF_MILLIS;
#endif
}

void UITask::refreshContext() {
  _ctx.msg_count = _msgcount;
  _ctx.bt_enabled = isBluetoothEnabled();
  _ctx.connected = hasConnection();
  _ctx.buzzer_muted = isBuzzerQuiet();
  _bt_status = _ctx.connected ? "linked" : (_ctx.bt_enabled ? "on" : "off");
  _ctx.node_name = _node_prefs ? _node_prefs->node_name : "";
#if ENV_INCLUDE_GPS == 1
  _ctx.gps_on = _node_prefs && _node_prefs->gps_enabled;
#endif
  _neigh.setTitle(_ctx.node_name);
}

void UITask::sendAdvert(bool flood) {
  /* Both kinds are worth having and they are very different on the air: a
     zero-hop advert reaches whoever can hear this radio, a flood crosses the
     mesh. The toast names which one went out -- "advert sent" would hide the
     distinction that made two gestures worth spending. */
  bool ok = the_mesh.advert(flood);
  notify(UIEventType::ack);
  _screen.showToast(ok ? (flood ? "flood advert sent" : "0-hop advert sent")
                       : "advert failed", millis());
  _next_render = 0;
}

void UITask::toggleBluetooth() {
  if (isBluetoothEnabled()) {
    disableBluetooth();
    _screen.showToast("bluetooth off", millis());
  } else {
    enableBluetooth();
    _screen.showToast("bluetooth on", millis());
  }
  notify(UIEventType::ack);
  _next_render = 0;
}

void UITask::doShutdown() {
#ifdef PIN_BUZZER
  buzzer.shutdown();
  uint32_t t = millis();
  while (buzzer.isPlaying() && (millis() - 2500) < t) buzzer.loop();
#endif
  if (_display != NULL) _display->turnOff();
  _board->powerOff();
}

void UITask::pageAction(bool heavy) {
  uint32_t now = millis();
  int idx = _screen.currentPage();

  /* Dispatch on the index recorded when the page was added, not arithmetic over
     numPages(): the GPS page is conditional, so counting back from the end
     pointed at the wrong page in builds without it. */
  if (idx == _idx_advert) {
    sendAdvert(heavy);                       // tap 0-hop, hold flood
    return;
  }
  if (idx == _idx_bt && !heavy) {
    toggleBluetooth();
    return;
  }
  if (idx == _idx_shutdown && heavy) {
    doShutdown();                            // does not return
    return;
  }
  if (idx == _idx_home && !heavy) {
    if (_node_prefs) {
      _node_prefs->buzzer_quiet = _node_prefs->buzzer_quiet ? 0 : 1;
  #ifdef PIN_BUZZER
      buzzer.quiet(_node_prefs->buzzer_quiet != 0);
  #endif
      the_mesh.savePrefs();
      _screen.showToast(_node_prefs->buzzer_quiet ? "buzzer muted" : "buzzer on", now);
    }
    _next_render = 0;
    return;
  }
#if ENV_INCLUDE_GPS == 1
  if (_idx_gps >= 0 && idx == _idx_gps && !heavy) {
    if (_node_prefs != NULL) {
      _node_prefs->gps_enabled = _node_prefs->gps_enabled ? 0 : 1;
      the_mesh.applyGpsPrefs();              // pushes the flag into SensorManager
      the_mesh.savePrefs();
      _screen.showToast(_node_prefs->gps_enabled ? "gps on" : "gps off", now);
    }
    _next_render = 0;
    return;
  }
#endif
  /* A page with no action of this weight says so, rather than leaving the
     press looking like a device that has stopped responding. */
  _screen.showToast(heavy ? "nothing to hold here" : "nothing to tap here", now);
  _next_render = 0;
}

void UITask::pollButtons() {
  /* Poll BOTH every pass. MomentaryButton's multi-click window only advances
     while check() is being called, so returning early on one button's event
     stalls the other's detection. */
  int ev1 = user_btn.check();
  int ev2 = _btn2.check();
#ifdef UI_BUTTON_DEBUG
  if (ev1) Serial.printf("BTN b1(%d) ev=%d\n", PIN_USER_BTN, ev1);
  if (ev2) Serial.printf("BTN b2(%d) ev=%d\n", UI_BUTTON2_PIN, ev2);
#endif
  if (ev1 == BUTTON_EVENT_NONE && ev2 == BUTTON_EVENT_NONE) return;

  char key = 0;
  switch (ev1) {
    case BUTTON_EVENT_CLICK:        key = KEY_NEXT; break;
    case BUTTON_EVENT_DOUBLE_CLICK: key = KEY_PREV; break;
    case BUTTON_EVENT_LONG_PRESS:   key = KEY_HOME; break;
    default: break;
  }
  if (key != 0) {
    _screen.handleInput(key);
  } else {
    switch (ev2) {
      case BUTTON_EVENT_CLICK:      pageAction(false); break;   // light
      case BUTTON_EVENT_LONG_PRESS: pageAction(true);  break;   // heavy
      default: break;
    }
  }
  _next_render = 0;
#if AUTO_OFF_MILLIS > 0
  if (_display != NULL && !_display->isOn()) _display->turnOn();
  _auto_off = millis() + AUTO_OFF_MILLIS;
#endif
}

void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
  _next_render = 0;
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  _msgcount = msgcount;
  _recent.add(path_len, from_name, text, millis());
  /* No screen takeover: the message lands on the messages page and the count
     on the home page. A page that seizes the display would also seize it while
     the device is in a pocket, and on e-paper it would then sit there. */
  _next_render = 0;
#if AUTO_OFF_MILLIS > 0
  if (_display != NULL && !_display->isOn() && !hasConnection()) _display->turnOn();
  _auto_off = millis() + AUTO_OFF_MILLIS;
#endif
}

void UITask::notify(UIEventType t) {
#ifdef PIN_BUZZER
  if (isBuzzerQuiet()) return;
  switch (t) {
    case UIEventType::contactMessage:
      // Direct messages only. A channel can be busy, and a node that chirps at
      // every channel message on a shared frequency becomes one you mute.
      buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
      break;
    case UIEventType::ack:
      buzzer.play("ack:d=32,o=8,b=120:c");
      break;
    default:
      break;
  }
#else
  (void)t;
#endif
}

void UITask::loop() {
#ifdef PIN_BUZZER
  buzzer.loop();
#endif
  if (!_ok || _display == NULL) return;

  pollButtons();
  _screen.poll();

  uint32_t now = millis();
  _power.sample(now, _board->getBattMilliVolts());

#if AUTO_OFF_MILLIS > 0
  if (_display->isOn() && !hasConnection() && (int32_t)(now - _auto_off) > 0) {
    _display->turnOff();
  }
#endif
  if (!_display->isOn()) return;
  if ((int32_t)(now - _next_render) < 0) return;

  refreshContext();
  _neighbours.refresh(now);
  _next_render = now + _screen.render(*_display, now);
}
