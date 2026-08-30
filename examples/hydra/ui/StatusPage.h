#pragma once

#include <helpers/ui/PagedScreen.h>
#include <helpers/PowerMonitor.h>
#include "../HydraNode.h"
#include "../SlotPolicy.h"

/* The node itself: how long it has been up, what the pack is doing, what the
   silicon is doing, and what the radio has moved. Everything here is read
   straight from something that already existed -- this page owns no state
   except the power trend, which nothing else was tracking. */
class StatusPage : public UIPage {
  PowerMonitor& _power;
  int _pitch;

  static void fmtUptime(char* out, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000, d = s / 86400;
    s %= 86400;
    if (d) snprintf(out, n, "%ud %02u:%02u", (unsigned)d, (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
    else   snprintf(out, n, "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  }

  static void fmtDuration(char* out, size_t n, int32_t mins) {
    if (mins < 0)      snprintf(out, n, "?");
    else if (mins < 90) snprintf(out, n, "%dm", (int)mins);
    else if (mins < 48 * 60) snprintf(out, n, "%dh%02dm", (int)(mins / 60), (int)(mins % 60));
    else               snprintf(out, n, "%dd", (int)(mins / (60 * 24)));
  }

  void row(DisplayDriver& d, int y, const char* left, const char* right) {
    d.drawTextLeftAlign(0, y, left);
    if (right) d.drawTextRightAlign(d.width(), y, right);
  }

public:
  StatusPage(PowerMonitor& power, int pitch) : _power(power), _pitch(pitch) { }

  int renderBody(DisplayDriver& display, int avail_h) override {
    char l[28], r[28];
    int y = 2;
    const int W = display.width();

    display.setColor(UIColor::primary_txt);
    display.drawTextLeftAlign(0, y, "NODE");
    fmtUptime(r, sizeof(r), millis());
    display.drawTextRightAlign(W, y, r);
    y += _pitch;

    // divider, as text: fillRect does not share an origin with text on GxEPD
    const int dash_w = display.getTextWidth("-");
    int dashes = dash_w > 0 ? W / dash_w : 0;
    if (dashes > (int)sizeof(l) - 1) dashes = (int)sizeof(l) - 1;
    for (int i = 0; i < dashes; i++) l[i] = '-';
    l[dashes > 0 ? dashes : 0] = 0;
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(0, y, l);
    display.setColor(UIColor::primary_txt);
    y += _pitch;

    int pct = _power.percent();
    if (pct < 0) {
      row(display, y, "batt", "n/a");
    } else {
      snprintf(l, sizeof(l), "batt %d%%", pct);
      snprintf(r, sizeof(r), "%u.%02uV",
               (unsigned)(_power.millivolts() / 1000),
               (unsigned)((_power.millivolts() % 1000) / 10));
      row(display, y, l, r);
      y += _pitch;
      if (_power.isCharging()) {
        row(display, y, "  charging", NULL);
      } else {
        int32_t mins = _power.minutesRemaining();
        fmtDuration(r, sizeof(r), mins);
        // Say "estimating" rather than a fake number: the trend needs ~10 min.
        row(display, y, "  left", mins < 0 ? "est.." : r);
      }
    }
    y += _pitch;

    float t = board.getMCUTemperature();
    if (t == t) {   // NaN check without <math.h>
      snprintf(r, sizeof(r), "%d.%dC", (int)t, (int)((t < 0 ? -t : t) * 10) % 10);
      row(display, y, "temp", r);
      y += _pitch;
    }

#if defined(ESP32)
    // Only the ESP32 scales its clock; the nRF52840's M4 is fixed at 64MHz, so
    // that row would be a constant and is not worth one of nine lines.
    snprintf(r, sizeof(r), "%uMHz", (unsigned)getCpuFrequencyMhz());
    row(display, y, "cpu", r);
    y += _pitch;
#endif

    /* The largest block the allocator will still hand out -- the same probe the
       `slots` command reports, not a free-heap total. On a node that allocates
       an ACL and a post buffer per enabled slot, contiguity is what actually
       runs out, and two numbers that disagree would be worse than one. */
    snprintf(r, sizeof(r), "%uk", (unsigned)(probeLargestBlock(64 * 1024) / 1024));
    row(display, y, "block", r);
    y += _pitch;

    uint32_t rx = 0, tx = 0;
    for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
      rx += hydra.radio().portRxCount(i);
      tx += hydra.radio().portTxCount(i);
    }
    snprintf(l, sizeof(l), "rx %u", (unsigned)rx);
    snprintf(r, sizeof(r), "tx %u", (unsigned)tx);
    row(display, y, l, r);
    y += _pitch;

    snprintf(l, sizeof(l), "noise %d", hydra.repeater().port().getNoiseFloor());
    uint32_t air_s = hydra.radio().airtimeMs() / 1000;
    snprintf(r, sizeof(r), "air %us", (unsigned)air_s);
    if (y + _pitch <= avail_h) row(display, y, l, r);

    return 5000;
  }
};
