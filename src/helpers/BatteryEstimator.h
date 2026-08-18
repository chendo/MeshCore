#pragma once

#include <Arduino.h>

/**
 * @brief  Charge/discharge rate and runtime estimates from battery voltage alone.
 *
 * There is no current sensing on these boards -- the only analog input wired to
 * the battery is a divider to one ADC channel -- so current cannot be measured.
 * It can be INFERRED, because we know the chemistry and the capacity:
 *
 *     I (mA) = capacity_mAh x d(state-of-charge) / dt
 *
 * The trick is to differentiate STATE OF CHARGE, not voltage. Extrapolating
 * volts linearly is wrong and wrong in the dangerous direction: a lithium cell's
 * curve is flat through the middle and steepens sharply at the bottom, so a
 * "millivolts per hour" figure taken at 3.8V badly UNDER-estimates how soon a
 * node will hit its cutoff. Converting to SoC first removes that.
 *
 * Accuracy, honestly:
 *
 *   - Best near the ends of the curve, where mV per percent is large.
 *   - Worst between about 3.70 and 3.95V, where the cell gives up 40% of its
 *     charge across ~250mV. Expect wide error bars there; sampleQuality()
 *     reports it rather than pretending otherwise.
 *   - Voltage is read under load, so it sits below open-circuit by I x ESR.
 *     At tens of milliamps and ~100mOhm that is a few mV -- negligible against
 *     the ADC's own noise.
 *   - While charging, the reading is the charger holding the terminal up, so
 *     the inferred figure is NET (charger minus the node's own draw). That is
 *     usually the number you actually want: it is what decides whether the
 *     battery is winning.
 *
 *   - It CANNOT see constant-voltage charging. Near full, a charger stops
 *     raising the voltage and tapers the current instead, so dV/dt goes to zero
 *     while real current is still flowing. This will report ~0mA and "full"
 *     long before the cell actually is. Treat a stalled reading above ~4.1V as
 *     "in CV, still charging", not "finished".
 *   - While charging, the terminal sits ABOVE the resting voltage by I x ESR,
 *     so the absolute state of charge reads high. The derivative is largely
 *     unaffected while the current is steady -- the offset cancels between the
 *     two endpoints -- which is why the mA figure is more trustworthy than the
 *     percentage during a charge.
 *
 * Deliberately long window. The rates involved are tens of millivolts per HOUR
 * against an ADC that jitters a few millivolts sample to sample, so a short
 * window measures noise. A least-squares fit over hours is what makes the
 * estimate meaningful.
 */
class BatteryEstimator {
public:
  /* One sample a minute over two hours. Enough span to resolve ~10mV/hr against
     ADC noise, and 240 bytes of RAM. */
  static const uint8_t  MAX_SAMPLES = 120;
  static const uint32_t SAMPLE_INTERVAL_MS = 60000;

  /** @param capacity_mah  cell capacity; 0 means unknown, and current/power
   *                       are then not reported (percentages still are). */
  void begin(uint16_t capacity_mah) { _capacity = capacity_mah; }

  /** True when a new sample is due. Check this BEFORE reading the ADC: the
   *  main loop runs tens of thousands of times a second and an ADC conversion
   *  is not free, so the read itself must be paced, not just the storing of
   *  its result. */
  bool due() const {
    if (_n == 0) return true;
    return (long)(millis() - _last_ms) >= (long)SAMPLE_INTERVAL_MS;
  }

  /** Feed the battery voltage. Call only when due(). */
  void update(uint16_t mv) {
    if (mv < 2000 || mv > 5000) return;              // not a plausible cell
    uint32_t now = millis();
    if (_n != 0 && (long)(now - _last_ms) < (long)SAMPLE_INTERVAL_MS) return;
    _last_ms = now;
    _mv[_head] = mv;
    _head = (uint8_t)((_head + 1) % MAX_SAMPLES);
    if (_n < MAX_SAMPLES) _n++;
  }

  uint8_t numSamples() const { return _n; }
  uint16_t latestMv() const {
    if (_n == 0) return 0;
    return _mv[(uint8_t)((_head + MAX_SAMPLES - 1) % MAX_SAMPLES)];
  }

  /** State of charge, 0-100, from the latest reading. */
  uint8_t percent() const { return socFromMv(latestMv()); }

  /** Millivolts per hour, signed. Least-squares over the whole window. */
  int32_t mvPerHour() const {
    if (_n < 4) return 0;
    /* x is the sample index, which is time in minutes because the interval is
       fixed -- so the slope is mV per minute, scaled to the hour below. */
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint8_t i = 0; i < _n; i++) {
      float x = (float)i;
      float y = (float)at(i);
      sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    float d = (_n * sxx) - (sx * sx);
    if (d == 0) return 0;
    float slope = ((_n * sxy) - (sx * sy)) / d;      // mV per sample
    return (int32_t)(slope * 60.0f);                 // mV per hour
  }

  /** Net current, mA. Positive = charging. 0 if capacity unknown or no trend.
   *
   *  Derived by converting the endpoints of the fitted trend to SoC, so the
   *  non-linearity of the curve is accounted for at the voltage we are actually
   *  sitting at rather than assumed away.
   */
  int32_t milliAmps() const {
    if (_capacity == 0 || _n < 4) return 0;
    int32_t dv = mvPerHour();
    if (dv == 0) return 0;
    int32_t now_mv = (int32_t)latestMv();
    int32_t then_mv = now_mv - dv;                   // where it was an hour ago
    if (then_mv < 2000) then_mv = 2000;
    if (then_mv > 5000) then_mv = 5000;
    /* Percent per hour, in tenths to keep a little resolution. */
    int32_t dsoc10 = (int32_t)soc10FromMv((uint16_t)now_mv)
                   - (int32_t)soc10FromMv((uint16_t)then_mv);
    return (dsoc10 * (int32_t)_capacity) / 1000;
  }

  /** Net power in milliwatts, signed. */
  int32_t milliWatts() const {
    int32_t ma = milliAmps();
    if (ma == 0) return 0;
    return (ma * (int32_t)latestMv()) / 1000;
  }

  /** Hours until full (charging) or until `floor_mv` (discharging).
   *  Returns -1 when it cannot be answered: no trend, or trending the wrong way. */
  int32_t hoursRemaining(uint16_t floor_mv) const {
    if (_n < 4) return -1;
    int32_t ma = milliAmps();
    if (ma == 0 || _capacity == 0) return -1;
    int32_t soc10 = (int32_t)soc10FromMv(latestMv());
    if (ma > 0) {
      int32_t togo = (1000 - soc10) * (int32_t)_capacity / 1000;   // mAh to full
      return togo / ma;
    }
    int32_t floor10 = (int32_t)soc10FromMv(floor_mv);
    if (soc10 <= floor10) return 0;
    int32_t togo = (soc10 - floor10) * (int32_t)_capacity / 1000;
    return togo / (-ma);
  }

  /** How much to trust the numbers: 2 good, 1 fair, 0 poor.
   *  Poor means we are on the flat part of the curve, where a large change in
   *  charge moves the voltage very little and any inference is soft. */
  uint8_t sampleQuality() const {
    if (_n < MAX_SAMPLES / 4) return 0;
    uint16_t mv = latestMv();
    if (mv >= 3700 && mv <= 3950) return _n >= MAX_SAMPLES ? 1 : 0;
    return _n >= MAX_SAMPLES / 2 ? 2 : 1;
  }

private:
  uint16_t _mv[MAX_SAMPLES];
  uint8_t _head = 0, _n = 0;
  uint32_t _last_ms = 0;
  uint16_t _capacity = 0;

  /* Oldest-first access into the ring. */
  uint16_t at(uint8_t i) const {
    uint8_t start = (_n == MAX_SAMPLES) ? _head : 0;
    return _mv[(uint8_t)((start + i) % MAX_SAMPLES)];
  }

  /* Discharge curve for a single lithium-polymer cell at a light load, in
     tenths of a percent. Coarse on purpose: the curve varies between cells and
     with temperature, and false precision here would be worse than none. The
     shape is what matters -- flat through the middle, steep at both ends. */
  struct Point { uint16_t mv; uint16_t soc10; };
  static uint16_t soc10FromMv(uint16_t mv) {
    static const Point CURVE[] = {
      { 3000,   0 }, { 3300,  50 }, { 3400,  80 }, { 3500, 100 },
      { 3600, 150 }, { 3650, 200 }, { 3700, 250 }, { 3750, 350 },
      { 3800, 450 }, { 3850, 550 }, { 3900, 650 }, { 4000, 800 },
      { 4100, 900 }, { 4200, 1000 },
    };
    const uint8_t N = sizeof(CURVE) / sizeof(CURVE[0]);
    if (mv <= CURVE[0].mv) return 0;
    if (mv >= CURVE[N - 1].mv) return 1000;
    for (uint8_t i = 1; i < N; i++) {
      if (mv <= CURVE[i].mv) {
        uint32_t span = CURVE[i].mv - CURVE[i - 1].mv;
        uint32_t into = mv - CURVE[i - 1].mv;
        uint32_t rise = CURVE[i].soc10 - CURVE[i - 1].soc10;
        return (uint16_t)(CURVE[i - 1].soc10 + (into * rise) / span);
      }
    }
    return 1000;
  }
  static uint8_t socFromMv(uint16_t mv) { return (uint8_t)(soc10FromMv(mv) / 10); }
};
