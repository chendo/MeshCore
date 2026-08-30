#pragma once

#include <stdint.h>

/**
 * \brief  Battery state derived from nothing but a voltage.
 *
 * Neither the M1 nor the M5 exposes a charge pin, and mesh::MainBoard offers
 * only getBattMilliVolts(), so "charging" and "time left" cannot be read -- they
 * have to be INFERRED from how the voltage moves. That inference is the whole
 * reason this is its own class: it is the only part of the status page with
 * logic worth testing, and it is pure arithmetic over (time, millivolts).
 *
 * Everything here is deliberately slow. A battery does not move quickly, the
 * ADC is noisy, and a reading taken while the radio keys up sags well below the
 * resting voltage. So samples are rate-limited, smoothed, and compared across a
 * window of many minutes rather than between consecutive reads.
 */
class PowerMonitor {
public:
  static const uint16_t DEFAULT_MIN_MV = 3000;   // matches UITask's battery icon
  static const uint16_t DEFAULT_MAX_MV = 4200;

  static const uint32_t SAMPLE_MS = 30000;              // 30s between accepted samples
  static const uint32_t TREND_MS  = 10 * 60 * 1000UL;   // compare across 10 minutes
  // A charge shows as a rise clear of ADC noise and of the recovery bounce that
  // follows a transmission. Below this, "flat" is the honest answer.
  static const int16_t  CHARGE_RISE_MV = 20;
  // Smoothing weight, as a shift: new = old + (raw - old) / 8.
  static const uint8_t  EMA_SHIFT = 3;

  PowerMonitor()
    : _min_mv(DEFAULT_MIN_MV), _max_mv(DEFAULT_MAX_MV), _ema_mv(0), _have(false),
      _next_sample(0), _anchor_ms(0), _anchor_mv(0),
      _delta_mv(0), _delta_ms(0), _have_trend(false) { }

  void begin(uint16_t min_mv, uint16_t max_mv) {
    if (max_mv > min_mv) { _min_mv = min_mv; _max_mv = max_mv; }
  }

  /** Safe to call every loop; it decides for itself when to take a reading.
   *  \param mv  0 means "no battery reading available" and is ignored. */
  void sample(uint32_t now_ms, uint16_t mv);

  bool hasReading() const { return _have; }
  uint16_t millivolts() const { return _ema_mv; }

  /** 0..100, or -1 before the first reading. Linear in voltage: crude for a
   *  lithium cell, but it is what the rest of this tree already shows, and a
   *  status page that disagrees with the battery icon is worse than a rough one. */
  int percent() const;

  /** True while the smoothed voltage is climbing. Not a charger signal -- there
   *  is no charger signal to read. */
  bool isCharging() const { return _have_trend && _delta_mv >= CHARGE_RISE_MV; }

  /** Minutes until the pack reaches _min_mv at the present rate, or -1 when
   *  that is not yet knowable: no trend, charging, or a flat/rising voltage.
   *  Reporting a number early would be worse than reporting none. */
  int32_t minutesRemaining() const;

private:
  uint16_t _min_mv, _max_mv;
  uint16_t _ema_mv;
  bool     _have;
  uint32_t _next_sample;
  uint32_t _anchor_ms;
  uint16_t _anchor_mv;
  int16_t  _delta_mv;      // smoothed change across the last completed window
  uint32_t _delta_ms;      // how long that window actually was
  bool     _have_trend;
};
