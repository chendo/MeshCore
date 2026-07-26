#pragma once
#include <Arduino.h>

// Battery state of charge for the ThinkNode M5.
//
// The board has no fuel gauge — battery is a resistor divider into ADC pin 8
// (see ThinknodeM5Board::getBattMilliVolts) — so percentage has to come from a
// voltage curve. A generic Li-ion table is a poor fit here: it describes a
// RESTING cell, while every reading we take is under load, and this node's load
// is both constant and significant. Under load the pack sags, so a resting
// table reads several percent pessimistic across the whole range.
//
// This table is therefore MEASURED, from a full unattended discharge of this
// exact board running this exact firmware (2026-07-27, 610 samples at 30s
// intervals over 5.31 h, 3968 mV -> 3198 mV). Load was constant, so energy
// drains linearly in time; sampling voltage at even fractions of the elapsed
// discharge gives the curve directly, with no model in between.
//
// The empty end is measured, not assumed. The node brown-out reboots at
// ~3270 mV and cannot boot at all below ~3194 mV, so 3200 mV is a real 0%:
// there is no usable energy under it.
//
// KNOWN GAP: the discharge started at 3968 mV, so everything above that is an
// estimate. The observed window is taken to be the bottom BOTTOM_WINDOW_PCT of
// capacity, and V_FULL..3968 mV is interpolated across the remainder. Replace
// BATT_CURVE wholesale once a discharge from a full charge has been logged;
// the shape below 3968 mV is solid and should be kept.

// Highest voltage seen on a completed charge (constant-voltage plateau).
#define BATT_FULL_MV        4180
// Fraction of total capacity that sits below 3968 mV, where the measurement
// starts. From a generic Li-ion curve at 3.97 V; the one modelled number here.
#define BATT_WINDOW_PCT     73

struct BattPoint { uint16_t mv; uint8_t pct_of_window; };

// Descending by voltage. pct_of_window is percent of the MEASURED window
// remaining (100 = 3970 mV, 0 = 3200 mV); scaled to absolute below.
//
// Sampled at even fractions of elapsed discharge time with +-5 min smoothing,
// then thinned so no two points sit closer than 12 mV. That thinning matters:
// through the flat middle of the curve the raw data produced points 1 mV apart
// carrying five percentage points, which is ADC noise, and interpolating across
// it created cliffs where the real curve is smooth.
static const BattPoint BATT_CURVE[] = {
  {3970, 100}, {3955,  88}, {3938,  80}, {3924,  72}, {3908,  68},
  {3888,  62}, {3870,  58}, {3848,  52}, {3834,  50}, {3821,  48},
  {3805,  45}, {3782,  42}, {3759,  40}, {3737,  38}, {3714,  35},
  {3692,  32}, {3667,  30}, {3646,  28}, {3629,  25}, {3617,  22},
  {3599,  20}, {3581,  18}, {3556,  15}, {3522,  12}, {3477,  10},
  {3415,   8}, {3335,   5}, {3266,   2}, {3200,   0},
};
static const int BATT_CURVE_N = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);

// 0-100. Saturates at both ends rather than extrapolating past the data.
static inline uint8_t batteryPercent(uint16_t mv) {
  const uint16_t top_mv = BATT_CURVE[0].mv;              // 3968
  if (mv >= BATT_FULL_MV) return 100;
  if (mv <= BATT_CURVE[BATT_CURVE_N - 1].mv) return 0;

  if (mv > top_mv) {
    // Estimated region: linear from BATT_WINDOW_PCT at 3968 mV to 100% at full.
    uint32_t span = BATT_FULL_MV - top_mv;
    uint32_t up   = (uint32_t)(mv - top_mv) * (100 - BATT_WINDOW_PCT);
    return (uint8_t)(BATT_WINDOW_PCT + (span ? up / span : 0));
  }

  for (int i = 0; i < BATT_CURVE_N - 1; i++) {
    const BattPoint& a = BATT_CURVE[i];
    const BattPoint& b = BATT_CURVE[i + 1];
    if (mv <= a.mv && mv >= b.mv) {
      uint32_t dv = (uint32_t)(a.mv - b.mv);
      uint32_t win = dv ? b.pct_of_window +
                          ((uint32_t)(mv - b.mv) * (a.pct_of_window - b.pct_of_window)) / dv
                        : b.pct_of_window;
      return (uint8_t)((win * BATT_WINDOW_PCT) / 100);   // window -> absolute
    }
  }
  return 0;
}
