#pragma once

// Collects the readings of peer clocks that mesh::clockEstimate works on, and
// the newest timestamp that this node has put on the air (the replay floor that
// mesh::clockDecide refuses to cross). See helpers/ClockPolicy.h for the rules.
//
// The caller passes in millis() and its own clock reading, so this class has
// no hardware dependency and a host test covers it.

#include <stdint.h>
#include <string.h>
#include "ClockPolicy.h"

namespace mesh {

class ClockSampler {
public:
  /* Readings live in their own ring, keyed on the originator, so a recent
     spread of readings is kept rather than the history of each node. */
  static const uint8_t  CAPACITY = 24;
  static const uint32_t MAX_AGE_MS = 60UL * 60UL * 1000UL;   // 1 hour

  /* The largest rate that a crystal can produce. A peer whose clock moves
     faster than this is being SET by something, so its present value says
     nothing about the true time. */
  static const int32_t  MAX_SANE_DRIFT_S_PER_DAY = 50;
  /* Both clocks read to the second, so a drift verdict over a short span is
     mostly quantisation. Below this span the sampler abstains. */
  static const uint32_t DRIFT_MIN_SPAN_MS = 2UL * 60UL * 60UL * 1000UL;

  /**
   * \brief  Record one advert's timestamp.
   * \param  pub_key   the originator's public key (the first 4 bytes are used)
   * \param  their_ts  the timestamp the originator signed
   * \param  hops      the path length the advert arrived with; 0 = direct
   * \param  now_ms    millis() now
   */
  void noteAdvert(const uint8_t* pub_key, uint32_t their_ts, uint8_t hops, uint32_t now_ms) {
    int idx = -1, oldest = 0;
    for (int i = 0; i < _count; i++) {
      if (memcmp(_r[i].pub4, pub_key, 4) == 0) { idx = i; break; }
      if ((uint32_t)(now_ms - _r[i].ms) > (uint32_t)(now_ms - _r[oldest].ms)) oldest = i;
    }
    if (idx < 0) {
      idx = (_count < CAPACITY) ? _count++ : oldest;
      memset(&_r[idx], 0, sizeof(_r[idx]));
      memcpy(_r[idx].pub4, pub_key, 4);
    } else {
      // The same node again. Keep the reading this one replaces for a rate
      // estimate, but only once it is far enough back to mean something.
      Reading& s = _r[idx];
      if (!s.has_prev || (uint32_t)(s.ms - s.prev_ms) >= DRIFT_MIN_SPAN_MS) {
        s.prev_their_ts = s.their_ts;
        s.prev_ms = s.ms;
        s.has_prev = true;
      }
      // A recent shorter path needs less correction; a longer copy must not
      // replace it.
      if (hops > s.hops && (uint32_t)(now_ms - s.ms) < MAX_AGE_MS / 4) return;
    }
    Reading& s = _r[idx];
    s.their_ts = their_ts;
    s.hops = hops;
    s.ms = now_ms;
  }

  /** \brief  Record the timestamp of an advert that THIS node transmitted. */
  void noteSent(uint32_t ts) {
    if (_sent_high_s == 0 || (int32_t)(ts - _sent_high_s) > 0) _sent_high_s = ts;
  }

  /**
   * \brief  The newest timestamp this node has put on the air; 0 = none yet.
   *
   * Peers drop an advert that is not newer than the one they last stored for
   * us, so mesh::clockDecide will not take the clock to or below this.
   */
  uint32_t sentHighWater() const { return _sent_high_s; }

  /**
   * \brief  Turn the ring into samples for mesh::clockEstimate.
   *
   * Drops readings that are too old, too far away, from a peer whose own clock
   * is unset, or from a peer whose apparent rate is impossible. Ages each
   * reading forward and subtracts against our clock NOW, never earlier, so a
   * step of our own clock cannot leave a stale difference behind. Peers that
   * share one corrected offset collapse onto one sample weighted by the group
   * size, so a sub-network synced from one source votes once.
   *
   * \returns  the number of samples written to `out`
   */
  int samples(ClockSample* out, int max, uint32_t our_now_s, uint32_t now_ms,
              uint16_t hop_delay_ms = CLOCK_HOP_DELAY_DEFAULT_MS) const {
    if (out == nullptr || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < _count && n < max; i++) {
      const Reading& s = _r[i];
      if ((uint32_t)(now_ms - s.ms) > MAX_AGE_MS) continue;
      if (s.hops > CLOCK_MAX_HOPS) continue;

      uint32_t theirs_now = s.their_ts + (uint32_t)(now_ms - s.ms) / 1000;
      if (clockIsUnset(theirs_now)) continue;   // an unset clock cannot help set ours

      if (s.has_prev) {
        uint32_t span = (uint32_t)(s.ms - s.prev_ms);
        if (span >= DRIFT_MIN_SPAN_MS) {
          int64_t slip = (int64_t)(int32_t)(s.their_ts - s.prev_their_ts) * 1000LL - (int64_t)span;
          int64_t per_day = (slip * 86400LL) / (int64_t)span;
          if (per_day > MAX_SANE_DRIFT_S_PER_DAY || per_day < -MAX_SANE_DRIFT_S_PER_DAY) continue;
        }
      }

      int32_t off = (int32_t)(theirs_now - our_now_s);
      int32_t corrected = off + clockHopCorrectionS(s.hops, hop_delay_ms);
      bool dup = false;
      for (int j = 0; j < n; j++) {
        if (out[j].offset_s + clockHopCorrectionS(out[j].hops, hop_delay_ms) != corrected) continue;
        if (out[j].weight < 255) out[j].weight++;
        if (s.hops < out[j].hops) { out[j].hops = s.hops; out[j].offset_s = off; }
        dup = true; break;
      }
      if (dup) continue;
      out[n].offset_s = off;
      out[n].hops = s.hops;
      out[n].weight = 1;
      n++;
    }
    return n;
  }

private:
  struct Reading {
    uint8_t  pub4[4];
    uint32_t their_ts;
    uint32_t ms;
    uint32_t prev_their_ts;
    uint32_t prev_ms;
    uint8_t  hops;
    bool     has_prev;
  };
  Reading  _r[CAPACITY];
  uint8_t  _count = 0;
  uint32_t _sent_high_s = 0;
};

}
