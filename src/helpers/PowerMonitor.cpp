#include "PowerMonitor.h"

const uint16_t PowerMonitor::DEFAULT_MIN_MV;
const uint16_t PowerMonitor::DEFAULT_MAX_MV;
const uint32_t PowerMonitor::SAMPLE_MS;
const uint32_t PowerMonitor::TREND_MS;
const int16_t  PowerMonitor::CHARGE_RISE_MV;
const uint8_t  PowerMonitor::EMA_SHIFT;

void PowerMonitor::sample(uint32_t now_ms, uint16_t mv) {
  if (mv == 0) return;                       // board has no battery sense

  if (!_have) {
    _have = true;
    _ema_mv = mv;
    _anchor_ms = now_ms;
    _anchor_mv = mv;
    _next_sample = now_ms + SAMPLE_MS;
    return;
  }

  /* Signed compare, so a millis() wrap costs one late sample instead of
     stalling the monitor for 49 days. */
  if ((int32_t)(now_ms - _next_sample) < 0) return;
  _next_sample = now_ms + SAMPLE_MS;

  int32_t ema = (int32_t)_ema_mv;
  ema += ((int32_t)mv - ema) >> EMA_SHIFT;
  _ema_mv = (uint16_t)ema;

  uint32_t elapsed = now_ms - _anchor_ms;
  if (elapsed >= TREND_MS) {
    _delta_mv = (int16_t)((int32_t)_ema_mv - (int32_t)_anchor_mv);
    _delta_ms = elapsed;
    _have_trend = true;
    _anchor_ms = now_ms;
    _anchor_mv = _ema_mv;
  }
}

int PowerMonitor::percent() const {
  if (!_have) return -1;
  int32_t span = (int32_t)_max_mv - (int32_t)_min_mv;
  if (span <= 0) return -1;
  int32_t pct = ((int32_t)_ema_mv - (int32_t)_min_mv) * 100 / span;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (int)pct;
}

int32_t PowerMonitor::minutesRemaining() const {
  if (!_have_trend || _delta_ms == 0) return -1;
  if (_delta_mv >= 0) return -1;             // charging or flat: no end in sight

  int32_t drop_mv = -(int32_t)_delta_mv;
  int32_t headroom = (int32_t)_ema_mv - (int32_t)_min_mv;
  if (headroom <= 0) return 0;

  /* minutes = headroom / (drop per minute). Done as one division on scaled
     integers so a slow drain cannot round its rate down to zero and report an
     infinite runtime. */
  int32_t window_min = (int32_t)(_delta_ms / 60000UL);
  if (window_min <= 0) window_min = 1;
  return headroom * window_min / drop_mv;
}
