#pragma once

#include <SPI.h>
#include <Wire.h>

#define ENABLE_GxEPD2_GFX 0

#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_4C.h>
#include <GxEPD2_7C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <CRC32.h>

#include "DisplayDriver.h"

class GxEPDDisplay : public DisplayDriver {

#if defined(EINK_DISPLAY_MODEL)
  GxEPD2_BW<EINK_DISPLAY_MODEL, EINK_DISPLAY_MODEL::HEIGHT> display;
  const float scale_x  = EINK_SCALE_X; 
  const float scale_y  = EINK_SCALE_Y;
  const float offset_x = EINK_X_OFFSET;
  const float offset_y = EINK_Y_OFFSET;
#else
  GxEPD2_BW<GxEPD2_150_BN, 200> display;
  const float scale_x  = 1.5625f;
  const float scale_y  = 1.5625f;
  const float offset_x = 0;
  const float offset_y = 10;
#endif
  bool _init = false;
  bool _isOn = false;
  /* Text size 0 selects the built-in 5x7 GFX font, whose cursor origin is the
     top-left of the glyph, where every FreeSans font here puts it on the
     baseline. EINK_Y_OFFSET compensates for the baseline convention, so the
     compact font needs its own correction or it draws a line too low. Held in
     PANEL pixels, not display units, because it is a font metric. */
  int _y_px_adj = 0;
  static const int CLASSIC_FONT_BASELINE_PX = 7;
  uint16_t _curr_color;
  CRC32 display_crc;
  int last_display_crc_value = 0;

public:
#if defined(EINK_DISPLAY_MODEL)
  /* height() must report what is DRAWABLE. Every y is shifted down by
     EINK_Y_OFFSET before it reaches the panel, so the last offset rows of the
     128-unit space fall off the bottom. Reporting 128 makes a screen that sizes
     itself from height() run its final row off the panel. */
  GxEPDDisplay() : DisplayDriver(128, 128 - (int)(EINK_Y_OFFSET)), display(EINK_DISPLAY_MODEL(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)) {}
#else
  GxEPDDisplay() : DisplayDriver(128, 128 - 10), display(GxEPD2_150_BN(DISP_CS, DISP_DC, DISP_RST, DISP_BUSY)) {}
#endif

  bool begin();

  bool isOn() override { return _isOn; }
  bool isEink() override { return true; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;
};
