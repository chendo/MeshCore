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
 * ONE BUTTON. Measured, not assumed: across full press tests PIN_BUTTON2
 * produced no edge as either INPUT or INPUT_PULLUP, and PIN_GPS_SWITCH is the
 * GPS module's hardware kill line rather than a spare input -- upstream only
 * reads it to report "gps off(hw)". So every action lives on a page:
 *
 *   tap        next page
 *   double     do what THIS page says it does
 *   triple     previous page
 *   hold (>1s) back to the first page
 *
 * Advert, Bluetooth and power-off are therefore pages, each labelled with what
 * a double-tap will do. ui-new reaches the same arrangement on this board for
 * the same reason.
 *
 * The hold threshold is the variant's 1000ms, not ours: B1 IS the variant's own
 * user_btn object rather than a second one on the same pin.
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
  void pageAction();          // B2 double-tap
  void sendAdvert();          // B2 tap
  void toggleBluetooth();     // B2 hold
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
