#include "MeshObserver.h"

void MeshObserver::addSelfKey(const uint8_t* pub_key) {
  if (pub_key == nullptr || _num_self >= MAX_SELF) return;
  memcpy(_self[_num_self++], pub_key, 4);
}

void MeshObserver::reset() {
  _num_peers = 0;
  _evictions = _refused = 0;
  _frames = 0;
  _tx_ring_head = _tx_ring_count = 0;
  memset(_flood_sent, 0, sizeof(_flood_sent));
  memset(_flood_confirmed, 0, sizeof(_flood_confirmed));
  memset(_confirm_width, 0, sizeof(_confirm_width));
  memset(_hops, 0, sizeof(_hops));
  memset(_types, 0, sizeof(_types));
}

int MeshObserver::confirmedPeerCount() const {
  int n = 0;
  for (int i = 0; i < _num_peers; i++) if (_peers[i].heard_us > 0) n++;
  return n;
}

int MeshObserver::selfIndex(const uint8_t* hash, uint8_t width) const {
  for (int i = 0; i < _num_self; i++) {
    if (memcmp(hash, _self[i], width) == 0) return i;
  }
  return -1;
}

void MeshObserver::observeTx(const uint8_t* frame, int len, int stream) {
  if (frame == nullptr || len < 1) return;
  if (stream < 0 || stream >= MAX_STREAMS) return;
  // Only floods get relayed onward, so only they can ever be confirmed. Route
  // types 0 and 1 are the flooded ones.
  uint8_t route = frame[0] & 0x03;
  if (route != 0 && route != 1) return;

  _flood_sent[stream]++;
  if (_tx_ring_count < TX_RING) {
    _tx_ring[(_tx_ring_head + _tx_ring_count) % TX_RING] = { (uint32_t)millis(), (int8_t)stream, false };
    _tx_ring_count++;
  } else {                                  // ring full: drop the oldest
    _tx_ring[_tx_ring_head] = { (uint32_t)millis(), (int8_t)stream, false };
    _tx_ring_head = (_tx_ring_head + 1) % TX_RING;
  }
}

void MeshObserver::creditRelay(int stream, uint8_t hash_width) {
  if (hash_width >= 1 && hash_width <= 4) _confirm_width[hash_width - 1]++;
  // A 1-byte hash collides once every 256 packets, so on its own it is not
  // evidence anyone relayed us: tallied above, but never credited.
  if (hash_width < 2) return;

  uint32_t now = millis();
  for (int k = _tx_ring_count - 1; k >= 0; k--) {
    TxRecord& r = _tx_ring[(_tx_ring_head + k) % TX_RING];
    if (r.stream != stream || r.confirmed) continue;
    if ((uint32_t)(now - r.t_ms) > _confirm_window_ms) break;   // older ones are older still
    r.confirmed = true;
    if (stream >= 0 && stream < MAX_STREAMS) _flood_confirmed[stream]++;
    return;
  }
}

int MeshObserver::peerTier(const PeerEntry& e) const {
  if (e.heard_us > 0) return 3;                   // it has relayed us: proven
  if (e.direct_rx > 0) {
    if (e.snr_n > 0 && e.snr4_sum / (int32_t)e.snr_n >= STRONG_SNR_4) return 2;
    return 1;
  }
  return 0;
}

int MeshObserver::widthCount(int bytes) const {
  if (bytes < 1 || bytes > 3) return 0;
  int n = 0;
  for (int i = 0; i < _num_peers; i++) if (_peers[i].width == bytes) n++;
  return n;
}

int MeshObserver::evictionVictim(uint32_t now) const {
  int best = -1, best_tier = 0, best_wide = 0;
  uint32_t best_age = 0;
  for (int i = 0; i < _num_peers; i++) {
    const PeerEntry& e = _peers[i];
    int tier = peerTier(e);
    uint32_t age = now - e.last_ms;               // unsigned: wrap-safe
    // Everything we have actually heard is spared until it goes quiet. Only
    // relay-only sightings can be dropped while still fresh.
    if (tier > 0 && age < STALE_MS) continue;
    int wide = (e.width >= 2) ? 1 : 0;            // 1-byte hashes go first
    if (best < 0 || tier < best_tier ||
        (tier == best_tier && wide < best_wide) ||
        (tier == best_tier && wide == best_wide && age > best_age)) {
      best = i; best_tier = tier; best_wide = wide; best_age = age;
    }
  }
  return best;
}

int MeshObserver::claimSlot(uint32_t now) {
  if (_num_peers < MAX_PEERS) return _num_peers++;
  int victim = evictionVictim(now);
  if (victim < 0) { _refused++; return -1; }      // all slots held by live peers
  _evictions++;
  return victim;
}

int MeshObserver::findPeer(const uint8_t* hash, uint8_t width) const {
  int found = -1;
  for (int i = 0; i < _num_peers; i++) {
    uint8_t cmp = width < _peers[i].width ? width : _peers[i].width;
    if (memcmp(hash, _peers[i].hash, cmp) != 0) continue;
    if (found >= 0) return -2;          // prefix matches several known peers
    found = i;
  }
  return found;
}

void MeshObserver::observeRx(const uint8_t* frame, int len, int8_t snr4) {
  if (frame == nullptr || len < 2) return;
  _frames++;
  _types[(frame[0] >> 2) & 0x0F]++;

  // hop depth of what we are hearing
  uint8_t route = frame[0] & 0x03;
  int o = 1;
  if (route == 0 || route == 3) o += 4;          // transport codes
  if (o < len) {
    uint8_t hops = frame[o] & 63;
    if (hops < HOP_BUCKETS) _hops[hops]++;
  }

  notePeersInPath(frame, len, snr4);
  noteAdvert(frame, len, snr4);
}

void MeshObserver::notePeersInPath(const uint8_t* frame, int len, int8_t snr4) {
  uint8_t route = frame[0] & 0x03;
  int o = 1;
  if (route == 0 || route == 3) o += 4;
  if (o >= len) return;
  uint8_t pl = frame[o++];
  uint8_t hops = pl & 63;
  uint8_t sz = (pl >> 6) + 1;
  if (hops == 0 || sz > 3 || o + hops * sz > len) return;

  uint32_t now = millis();
  for (uint8_t h = 0; h < hops; h++) {
    const uint8_t* hop = &frame[o + h * sz];
    // Our own hash only ever enters a path when WE forwarded the packet, so
    // hearing it come back at ANY position is proof the transmission
    // propagated — that is the confirmation. Attribution to a specific peer
    // needs the following entry, and is handled further down.
    int mine = selfIndex(hop, sz);
    if (mine >= 0) {
      creditRelay(mine, sz);
      continue;                                  // we are not our own peer
    }

    int idx = findPeer(hop, sz);
    if (idx == -2) continue;                     // ambiguous, attribute nothing
    if (idx < 0) {
      idx = claimSlot(now);
      if (idx < 0) continue;                     // every slot held by a live peer
      PeerEntry& n = _peers[idx];
      memset(&n, 0, sizeof(n));
      memcpy(n.hash, hop, sz);
      n.width = sz;
    } else if (sz > _peers[idx].width) {
      memcpy(_peers[idx].hash, hop, sz);         // a wider sighting refines it
      _peers[idx].width = sz;
    }

    PeerEntry& e = _peers[idx];
    e.relays++;
    e.last_ms = now;

    // Distance is position from the END: the last forwarder is one hop away.
    uint8_t dist = (uint8_t)(hops - h);
    if (e.min_hops == 0 || dist < e.min_hops) e.min_hops = dist;

    if (h + 1 == hops) {          // final hop: this frame came off ITS radio
      e.direct_rx++;
      e.last_direct_ms = now;
      e.snr4_sum += snr4;         // only these sightings describe our link to it
      e.snr_n++;
    }

    // Did it forward something WE transmitted? Only the entry immediately after
    // one of ours proves that, and only at 2 bytes or wider.
    if (h > 0 && isSelf(&frame[o + (h - 1) * sz], sz)) {
      if (sz >= 2) e.heard_us++; else e.heard_us_1b++;   // credited above
    }
  }
}

bool MeshObserver::parseAdvert(const uint8_t* frame, int len,
                               const uint8_t*& pub, uint8_t& hops,
                               uint32_t& their_ts) const {
  if (frame == nullptr || len < 2) return false;
  if (((frame[0] >> 2) & 0x0F) != 4) return false;    // PAYLOAD_TYPE_ADVERT

  uint8_t route = frame[0] & 0x03;
  int o = 1;
  if (route == 0 || route == 3) o += 4;
  if (o >= len) return false;
  uint8_t pl = frame[o++];
  hops = pl & 63;
  uint8_t sz = (pl >> 6) + 1;
  if (sz > 3 || o + hops * sz > len) return false;
  o += hops * sz;

  // payload: [pub_key 32][timestamp 4][signature 64][app_data]
  if (o + 100 > len) return false;
  pub = &frame[o];
  if (isSelf(pub, 4)) return false;                   // never record ourselves

  memcpy(&their_ts, &frame[o + 32], 4);
  return true;
}

void MeshObserver::observeBridgedAdvert(const uint8_t* frame, int len) {
  if (_clock == nullptr) return;

  const uint8_t* pub;
  uint8_t hops;
  uint32_t their_ts;
  if (!parseAdvert(frame, len, pub, hops, their_ts)) return;
  if (their_ts < MIN_SANE_EPOCH) return;

  /* Clock evidence ONLY -- see the header for why the peer table and the
     histograms are deliberately left alone. noteClockSample keys on the
     originator, so an advert reaching us over both the radio and the bridge
     updates one slot rather than counting as two agreeing sources. */
  noteSighting(pub, their_ts, hops);
  noteClockSample(pub, hops, their_ts);
}

void MeshObserver::noteAdvert(const uint8_t* frame, int len, int8_t snr4) {
  const uint8_t* pub;
  uint8_t hops;
  uint32_t their_ts;
  if (!parseAdvert(frame, len, pub, hops, their_ts)) return;

  // An empty path means we received the originator's own transmission, so it is
  // one hop away. Otherwise it sits beyond the forwarders that did relay it.
  uint8_t dist = (uint8_t)(hops + 1);

  // Time this advert, and take a clock reading from it, BEFORE the peer-table
  // rules below get a say. A table slot is only granted to a node within two
  // hops -- correct for a neighbour table, and fatal here: a repeater indoors
  // hears mostly distant traffic, and gating on the slot left the estimator
  // with nothing to work from at all.
  int32_t  clock_delta = 0;
  bool     have_clock = false;
  if (_clock != nullptr) {
    uint32_t ours = _clock->getCurrentTime();
    if (their_ts >= MIN_SANE_EPOCH) {
      noteSighting(pub, their_ts, hops);
      noteClockSample(pub, hops, their_ts);
      if (ours >= MIN_SANE_EPOCH) {                 // peer-table display only
        clock_delta = (int32_t)(their_ts - ours);
        have_clock = true;
      }
    }
  }

  int idx = findPeer(pub, 3);
  if (idx == -2) return;                              // ambiguous, leave alone
  if (idx < 0) {
    if (dist > 2) return;                             // only near nodes earn a slot
    idx = claimSlot(millis());
    if (idx < 0) return;                              // nothing evictable
    PeerEntry& n = _peers[idx];
    memset(&n, 0, sizeof(n));
    memcpy(n.hash, pub, 3);
    n.width = 3;
  }
  PeerEntry& e = _peers[idx];
  if (e.min_hops == 0 || dist < e.min_hops) e.min_hops = dist;
  e.last_ms = millis();
  if (hops == 0) {                                    // heard it on our own radio
    e.direct_rx++;
    e.last_direct_ms = e.last_ms;
    e.snr4_sum += snr4;
    e.snr_n++;
  }

  memcpy(e.pub, pub, sizeof(e.pub));

  // Their clock against ours, from this advert's signed timestamp. Zero hops
  // only — see PeerEntry::clock_delta_s for why a relayed advert cannot be
  // used. Both sides must believe they know the date, or the subtraction is
  // measuring "never synced" rather than drift.
  if (have_clock && hops == 0) {   // the peer table shows skew for neighbours only
    e.clock_delta_s = clock_delta;
    e.clock_ms = e.last_ms;
    e.clock_n++;
  }

  /* app_data sits after [pub_key 32][timestamp 4][signature 64]. parseAdvert
     handed back `pub`, which points at the start of that payload, so the
     offset it computed is recovered here rather than duplicated. */
  const uint8_t* ad = pub + 100;
  int ad_len = len - (int)(pub - frame) - 100;
  if (ad_len <= 0) return;
  uint8_t fl = ad[0];
  int i = 1;
  if ((fl & 0x10) && ad_len >= i + 8) {
    memcpy(&e.lat_e6, &ad[i], 4);
    memcpy(&e.lon_e6, &ad[i + 4], 4);
    i += 8;
  }
  if (fl & 0x20) i += 2;
  if (fl & 0x40) i += 2;
  if ((fl & 0x80) && ad_len > i) {
    int n = ad_len - i;
    if (n > (int)sizeof(e.name) - 1) n = sizeof(e.name) - 1;
    memcpy(e.name, &ad[i], n);
    e.name[n] = 0;
    for (int k = 0; k < n; k++) if ((uint8_t)e.name[k] < 0x20) { e.name[k] = 0; break; }
  }
}

void MeshObserver::noteSighting(const uint8_t* pub, uint32_t advert_ts, uint8_t hops) {
  uint32_t now = millis();
  int free_idx = -1, oldest = 0;
  for (int i = 0; i < _num_sightings; i++) {
    AdvertSighting& s = _sightings[i];
    if (memcmp(s.pub4, pub, 4) == 0 && s.advert_ts == advert_ts) {
      // A later copy of an advert we have already seen. Only a copy that came
      // further counts: an equal or shorter path is a different branch of the
      // flood, not another hop of the same one.
      if (hops > s.last_hops) {
        _hop_delay_sum_ms += (uint32_t)(now - s.last_ms);
        _hop_delay_hops   += (uint32_t)(hops - s.last_hops);
        _hop_delay_pairs++;
        s.last_ms = now;
        s.last_hops = hops;
      }
      return;
    }
    if ((uint32_t)(now - _sightings[i].last_ms) > (uint32_t)(now - _sightings[oldest].last_ms))
      oldest = i;
  }
  if (_num_sightings < ADVERT_SIGHTINGS) free_idx = _num_sightings++;
  else free_idx = oldest;
  AdvertSighting& s = _sightings[free_idx];
  memcpy(s.pub4, pub, 4);
  s.advert_ts = advert_ts;
  s.last_ms = now;
  s.last_hops = hops;
}

uint16_t MeshObserver::hopDelayMs() const {
  if (_hop_delay_pairs < HOP_DELAY_MIN_PAIRS || _hop_delay_hops == 0)
    return HOP_DELAY_DEFAULT_MS;
  uint32_t d = _hop_delay_sum_ms / _hop_delay_hops;
  return d > 60000 ? 60000 : (uint16_t)d;      // a minute a hop is already absurd
}

void MeshObserver::noteClockSample(const uint8_t* pub, uint8_t hops, uint32_t their_ts) {
  uint32_t now = millis();
  int idx = -1, oldest = 0;
  for (int i = 0; i < _num_clock_samples; i++) {
    if (memcmp(_clock_samples[i].pub4, pub, 4) == 0) { idx = i; break; }
    if ((uint32_t)(now - _clock_samples[i].ms) > (uint32_t)(now - _clock_samples[oldest].ms))
      oldest = i;
  }
  if (idx < 0) {
    idx = (_num_clock_samples < CLOCK_SAMPLES) ? _num_clock_samples++ : oldest;
    memset(&_clock_samples[idx], 0, sizeof(_clock_samples[idx]));
    memcpy(_clock_samples[idx].pub4, pub, 4);
  } else {
    // Same node again: carry the reading being replaced down so a rate can be
    // estimated, but only once the old one is far enough back to mean anything.
    ClockSample& s = _clock_samples[idx];
    if (s.ms != 0 && (s.prev_ms == 0 ||
        (uint32_t)(s.ms - s.prev_ms) >= DRIFT_MIN_SPAN_MS)) {
      s.prev_their_ts = s.their_ts;
      s.prev_ms = s.ms;
    }
    // Prefer the shortest path we have heard recently: a zero-hop reading needs
    // no correction at all, so do not let a relayed copy displace one.
    if (hops > s.hops && (uint32_t)(now - s.ms) < CLOCK_VOTE_MAX_AGE_MS / 4) return;
  }
  ClockSample& s = _clock_samples[idx];
  s.their_ts = their_ts;
  s.hops = hops;
  s.ms = now;
}

/* Sorted ascending in place, carrying the parallel arrays along. n is bounded
   by CLOCK_SAMPLES so an insertion sort beats qsort's comparator indirection. */
static void sortSamples(int32_t* a, uint8_t* z, uint8_t* cnt, int n) {
  for (int i = 1; i < n; i++) {
    int32_t v = a[i]; uint8_t vz = z[i], vc = cnt[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) {
      a[j+1] = a[j]; z[j+1] = z[j]; cnt[j+1] = cnt[j]; j--;
    }
    a[j+1] = v; z[j+1] = vz; cnt[j+1] = vc;
  }
}

static int32_t medianOfSorted(const int32_t* a, int n) {
  if (n <= 0) return 0;
  if (n & 1) return a[n / 2];
  // Truncates toward zero, so an even split either side of true time does not
  // manufacture a correction out of nothing.
  return (a[n / 2 - 1] + a[n / 2]) / 2;
}

MeshObserver::ClockConsensus MeshObserver::clockConsensus(uint8_t min_sources) const {
  ClockConsensus c;
  c.valid = false; c.offset_s = 0; c.n_seen = 0; c.n_used = 0;
  c.agree_pct = 0; c.spread_s = 0; c.n_zero_hop = 0;
  c.hop_delay_ms = hopDelayMs();

  int32_t d[CLOCK_SAMPLES];
  uint8_t z[CLOCK_SAMPLES];
  uint8_t cnt[CLOCK_SAMPLES];   // nodes collapsed into each distinct value
  int n = 0, nodes = 0;
  uint32_t now = millis();
  if (_clock == nullptr) return c;
  uint32_t ours = _clock->getCurrentTime();

  for (int i = 0; i < _num_clock_samples && n < CLOCK_SAMPLES; i++) {
    const ClockSample& s = _clock_samples[i];
    if (s.ms == 0) continue;
    if ((uint32_t)(now - s.ms) > CLOCK_VOTE_MAX_AGE_MS) continue;   // stale
    if (s.hops > MAX_CLOCK_HOPS) continue;

    uint32_t elapsed_s = (uint32_t)(now - s.ms) / 1000;
    uint32_t theirs_now = s.their_ts + elapsed_s;
    // A node that has not had its own clock set cannot help us set ours.
    if (theirs_now < CLOCK_SET_EPOCH) continue;

    // A rate no crystal can produce means that clock is being SET, not
    // drifting, and its current value says nothing about what time it is.
    // Measured as their elapsed seconds against our elapsed milliseconds, so
    // it stays honest even if our own clock was stepped in between.
    if (s.prev_ms != 0) {
      uint32_t span = (uint32_t)(s.ms - s.prev_ms);
      if (span >= DRIFT_MIN_SPAN_MS) {
        int64_t slip = (int64_t)(s.their_ts - s.prev_their_ts) * 1000LL - (int64_t)span;
        int64_t per_day = (slip * 86400000LL) / ((int64_t)span * 1000LL);
        if (per_day > MAX_SANE_DRIFT_S_PER_DAY ||
            per_day < -MAX_SANE_DRIFT_S_PER_DAY) continue;
      }
    }

    // A relayed advert was stamped before it set off, so it reads late by
    // however long the trip took. Undo that and the reading is as good as any
    // other -- the residual scatter grows only as sqrt(hops), which is well
    // inside the spread the mesh itself runs at, so nothing is down-weighted.
    int32_t corrected = (int32_t)(theirs_now - ours)
                      + (int32_t)(((uint32_t)s.hops * c.hop_delay_ms + 500) / 1000);

    nodes++;
    // Collapse a cluster to a single vote. Nodes sharing an upstream sync
    // source share its error exactly, so counting them individually lets one
    // wrong sub-network outvote the rest of the mesh on population alone.
    bool dup = false;
    for (int j = 0; j < n; j++) {
      if (d[j] == corrected) {
        if (s.hops == 0) z[j] = 1;
        if (cnt[j] < 255) cnt[j]++;
        dup = true; break;
      }
    }
    if (dup) continue;

    d[n] = corrected; z[n] = (s.hops == 0) ? 1 : 0; cnt[n] = 1; n++;
  }

  /* Quorum counts NODES, not distinct values. Collapsing a cluster is right for
     the statistics -- 41 nodes sharing one upstream error must not vote 41
     times -- but it must not also decide whether we have enough sources: at
     second granularity a handful of honest neighbours land on the same value
     often enough that quorum-by-value would refuse perfectly good data. */
  c.n_seen = (uint8_t)(nodes > 255 ? 255 : nodes);
  if (nodes < min_sources || n < 1) return c;

  sortSamples(d, z, cnt, n);
  int32_t med = medianOfSorted(d, n);

  // Median absolute deviation: the spread of the honest majority, and unlike a
  // standard deviation it does not care how extreme the outliers are.
  int32_t dev[CLOCK_SAMPLES];
  uint8_t dz[CLOCK_SAMPLES], dc[CLOCK_SAMPLES];
  for (int i = 0; i < n; i++) {
    int32_t v = d[i] - med; dev[i] = v < 0 ? -v : v; dz[i] = z[i]; dc[i] = cnt[i];
  }
  sortSamples(dev, dz, dc, n);
  int32_t mad = medianOfSorted(dev, n);

  // 3 * 1.4826 * MAD is the three-sigma equivalent for a normal core.
  int32_t limit = (int32_t)(((int64_t)mad * 4448) / 1000);
  if (limit < CLOCK_CLIP_FLOOR_S) limit = CLOCK_CLIP_FLOOR_S;

  int32_t kept[CLOCK_SAMPLES];
  int k = 0, kept_nodes = 0, zero_hop = 0;
  for (int i = 0; i < n; i++) {          // d is sorted, so kept stays sorted
    int32_t v = d[i] - med;
    if (v < 0) v = -v;
    if (v <= limit) { kept[k++] = d[i]; kept_nodes += cnt[i]; if (z[i]) zero_hop++; }
  }
  if (kept_nodes < min_sources) return c;

  c.offset_s   = medianOfSorted(kept, k);
  c.spread_s   = mad;
  c.n_used     = (uint8_t)(kept_nodes > 255 ? 255 : kept_nodes);
  c.n_zero_hop = (uint8_t)zero_hop;
  c.agree_pct  = (uint8_t)((kept_nodes * 100) / nodes);
  c.valid      = true;
  return c;
}
