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
 * Buttons (two on this board):
 *   B1 tap      next page
 *   B1 double   previous page
 *   B1 hold     first page
 *   B2 tap      send a flood advert      -- from ANY page
 *   B2 hold     toggle Bluetooth         -- from ANY page
 *   B2 double   the current page's action (mute, GPS, arm power-off)
 *
 * B2's two global gestures are why the page action is a double-tap: with the
 * tap and the hold already spoken for, there was no other gesture left, and the
 * pages that need one would otherwise have been unreachable.
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
#if ENV_INCLUDE_GPS == 1
  GpsPage          _gps;
#endif
  ShutdownPage     _shutdown;

  MomentaryButton  _btn1;
  MomentaryButton  _btn2;
#ifdef PIN_BUZZER
  // A member, not a global: every ui-* variant in this tree owns its own.
  genericBuzzer    buzzer;
#endif

  int       _msgcount;
  uint32_t  _next_render;
  uint32_t  _auto_off;
  char      _radio_sub[28];
  bool      _ok;
  int       _idx_home, _idx_gps, _idx_shutdown;

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
