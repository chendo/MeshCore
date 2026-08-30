#pragma once

#include "../AbstractUITask.h"
#include "Pages.h"
#include <helpers/ObservingWrapper.h>
#include <helpers/ObserverNeighbours.h>
#include <helpers/ui/MomentaryButton.h>
#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

/**
 * \brief  A paged companion UI, built on PagedScreen.
 *
 * A sibling of ui-new/ui-tiny/ui-orig, selected purely by the include path and
 * the source filter -- companion_radio's main.cpp does `#include "UITask.h"`,
 * so no upstream file changes to swap the UI. Everything companion_radio calls
 * on the concrete type is begin() and loop(); the rest arrives through
 * AbstractUITask.
 *
 * Two buttons, both confirmed against Elecrow's datasheet AND a press test:
 *
 *   B1 "Page Turn"  P1.10 = 42   tap next page, double previous, hold first
 *   B2 "Function"   P1.07 = 39   acts on the page you are looking at
 *
 * ONE RULE for B2, everywhere: tap is the LIGHT action, hold is the HEAVY one.
 * So a tap sends a zero-hop advert and a hold floods it; a hold powers the node
 * down. Nothing is global, so no page has to remember what a button means
 * somewhere else, and every page prints its own two labels -- declared by the
 * page itself, because labels and behaviour drift apart the moment they live in
 * different files.
 *
 * B2 is NOT PIN_BUTTON2. variant.h puts that at 11, which is wrong for this
 * board -- pin 11 produced no edge under either pull configuration, and nothing
 * upstream reads it here. The datasheet names P1.07, and that is what works.
 *
 * B1 is the variant's own `user_btn`, not a second object on the same pin. An
 * earlier version built its own with INPUT_PULLUP and saw nothing at all: the
 * internal pull-up holds that pin high straight through a press. Its hold
 * threshold is therefore the variant's 1000ms rather than ours.
 *
 * PIN_GPS_SWITCH is readable but is the GPS module's hardware kill line, not a
 * spare input -- upstream only reads it to report "gps off(hw)".
 */
class UITask : public AbstractUITask {
  DisplayDriver*  _display;
  SensorManager*  _sensors;
  NodePrefs*      _node_prefs;

  UIContext        _ctx;
  RecentMessages   _recent;
  PowerMonitor     _power;
  ObserverNeighbours _neighbours;

  PagedScreen      _screen;
  HomePage         _home;
  MessagesPage     _messages;
  NeighboursScreen _neigh;
  RadioPage        _radio;
  MomentaryButton  _btn2;
  ActionPage       _advert;
  ActionPage       _bluetooth;
  const char*      _bt_status;
#if ENV_INCLUDE_GPS == 1
  GpsPage          _gps;
#endif
  ShutdownPage     _shutdown;

  /* B1 is the variant's OWN user_btn (target.cpp), not a second object on the
     same pin. The variant knows how this board wires it -- plain INPUT, relying
     on an external pull-up. An earlier version built its own with INPUT_PULLUP
     and the button never once read low, while stock firmware on the same pin
     works. Do not duplicate a button the variant already declares. */
#ifdef PIN_BUZZER
  // A member, not a global: every ui-* variant in this tree owns its own.
  genericBuzzer    buzzer;
#endif

  int       _msgcount;
  uint32_t  _next_render;
  uint32_t  _auto_off;
  char      _radio_sub[28];
  bool      _ok;
  int       _idx_home, _idx_gps, _idx_shutdown, _idx_advert, _idx_bt;

  void refreshContext();
  void pollButtons();
  void pageAction(bool heavy);   // B2 tap (light) / hold (heavy)
  void sendAdvert(bool flood);
  void toggleBluetooth();
  void doShutdown();

public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* serial);

  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  bool hasDisplay() const { return _display != NULL; }
  int  getMsgCount() const { return _msgcount; }
#ifdef PIN_BUZZER
  bool isBuzzerQuiet() { return buzzer.isQuiet(); }
#else
  bool isBuzzerQuiet() { return true; }
#endif

  // AbstractUITask
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
};
