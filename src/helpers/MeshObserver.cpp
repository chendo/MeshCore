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
  // Other nodes relay only floods. Therefore only a flood can get a
  // confirmation. Route types 0 and 1 are the flooded types.
  uint8_t route = frame[0] & 0x03;
  if (route != 0 && route != 1) return;

  /* Record the newest timestamp that WE have put on the air. This is the
     replay floor that mesh::clockDecide refuses to take the clock below. An
     advert is the only thing a repeater sends that a peer replay-checks against
     a stored mark: BaseChatMesh compares it with ContactInfo::last_advert_
     timestamp and drops an advert that is not newer. Everything else that this
     node stamps goes through RTCClock::getCurrentTimeUnique, which holds a mark
     of its own. */
  {
    const uint8_t* pub;
    uint8_t hops;
    uint32_t ts;
    if (parseAdvertFrame(frame, len, pub, hops, ts) && isSelf(pub, 4)) {
      if (ts >= MIN_SANE_EPOCH && (int32_t)(ts - _sent_high_s) > 0) _sent_high_s = ts;
    }
  }

  _flood_sent[stream]++;
  if (_tx_ring_count < TX_RING) {
    _tx_ring[(_tx_ring_head + _tx_ring_count) % TX_RING] = { (uint32_t)millis(), (int8_t)stream, false };
    _tx_ring_count++;
  } else {                                  // the ring is full: discard the oldest
    _tx_ring[_tx_ring_head] = { (uint32_t)millis(), (int8_t)stream, false };
    _tx_ring_head = (_tx_ring_head + 1) % TX_RING;
  }
}

void MeshObserver::creditRelay(int stream, uint8_t hash_width) {
  if (hash_width >= 1 && hash_width <= 4) _confirm_width[hash_width - 1]++;
  // A 1-byte hash collides one time in every 256 packets. On its own it is
  // therefore not evidence that a node relayed us. The code counts it above,
  // but it never credits it.
  if (hash_width < 2) return;

  uint32_t now = millis();
  for (int k = _tx_ring_count - 1; k >= 0; k--) {
    TxRecord& r = _tx_ring[(_tx_ring_head + k) % TX_RING];
    if (r.stream != stream || r.confirmed) continue;
    if ((uint32_t)(now - r.t_ms) > _confirm_window_ms) break;   // the records before this one are older
    r.confirmed = true;
    if (stream >= 0 && stream < MAX_STREAMS) _flood_confirmed[stream]++;
    return;
  }
}

int MeshObserver::peerTier(const PeerEntry& e) const {
  if (e.heard_us > 0) return 3;                   // it has relayed us: this is proof
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
    uint32_t age = now - e.last_ms;               // unsigned: safe across a wrap
    // The code keeps every node that we have truly heard until that node goes
    // quiet. It can discard a fresh entry only if that entry has relay
    // sightings alone.
    if (tier > 0 && age < STALE_MS) continue;
    int wide = (e.width >= 2) ? 1 : 0;            // the 1-byte hashes go first
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
  if (victim < 0) { _refused++; return -1; }      // live peers hold all the slots
  _evictions++;
  return victim;
}

int MeshObserver::findPeer(const uint8_t* hash, uint8_t width) const {
  int found = -1;
  for (int i = 0; i < _num_peers; i++) {
    uint8_t cmp = width < _peers[i].width ? width : _peers[i].width;
    if (memcmp(hash, _peers[i].hash, cmp) != 0) continue;
    if (found >= 0) return -2;          // the prefix matches more than one known peer
    found = i;
  }
  return found;
}

void MeshObserver::observeRx(const uint8_t* frame, int len, int8_t snr4) {
  if (frame == nullptr || len < 2) return;
  _frames++;
  _types[(frame[0] >> 2) & 0x0F]++;

  // the hop depth of the traffic that we hear
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
    // Our own hash enters a path only when WE forwarded the packet. Therefore
    // our hash at ANY position in a returning path is proof that the
    // transmission propagated. That is the confirmation. To attribute the
    // relay to one peer, the code needs the entry that follows ours. The code
    // below does that.
    int mine = selfIndex(hop, sz);
    if (mine >= 0) {
      creditRelay(mine, sz);
      continue;                                  // we are not our own peer
    }

    int idx = findPeer(hop, sz);
    if (idx == -2) continue;                     // ambiguous: attribute nothing
    if (idx < 0) {
      idx = claimSlot(now);
      if (idx < 0) continue;                     // live peers hold every slot
      PeerEntry& n = _peers[idx];
      memset(&n, 0, sizeof(n));
      memcpy(n.hash, hop, sz);
      n.width = sz;
    } else if (sz > _peers[idx].width) {
      memcpy(_peers[idx].hash, hop, sz);         // a wider sighting improves it
      _peers[idx].width = sz;
    }

    PeerEntry& e = _peers[idx];
    e.relays++;
    e.last_ms = now;

    // The distance is the position from the END. The last forwarder is one hop
    // away.
    uint8_t dist = (uint8_t)(hops - h);
    if (e.min_hops == 0 || dist < e.min_hops) e.min_hops = dist;

    if (h + 1 == hops) {          // the final hop: this frame came off ITS radio
      e.direct_rx++;
      e.last_direct_ms = now;
      e.snr4_sum += snr4;         // only these sightings describe our link to it
      e.snr_n++;
    }

    // Did this node forward a frame that WE transmitted? Only the entry
    // directly after one of ours gives that proof, and only at a width of 2
    // bytes or more.
    if (h > 0 && isSelf(&frame[o + (h - 1) * sz], sz)) {
      if (sz >= 2) e.heard_us++; else e.heard_us_1b++;   // the code credits this above
    }
  }
}

bool MeshObserver::parseAdvert(const uint8_t* frame, int len,
                               const uint8_t*& pub, uint8_t& hops,
                               uint32_t& their_ts) const {
  if (!parseAdvertFrame(frame, len, pub, hops, their_ts)) return false;
  if (isSelf(pub, 4)) return false;                   // never record our own node
  return true;
}

bool MeshObserver::parseAdvertFrame(const uint8_t* frame, int len,
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

  /* Clock evidence ONLY. The header explains why this code does not change the
     peer table or the histograms. noteClockSample keys on the originator.
     Therefore an advert that reaches us over both the radio and the bridge
     updates one slot. It does not count as two sources that agree. */
  noteSighting(pub, their_ts, hops);
  noteClockSample(pub, hops, their_ts);
}

void MeshObserver::noteAdvert(const uint8_t* frame, int len, int8_t snr4) {
  const uint8_t* pub;
  uint8_t hops;
  uint32_t their_ts;
  if (!parseAdvert(frame, len, pub, hops, their_ts)) return;

  // An empty path means that we received the transmission of the originator
  // itself, so that node is one hop away. If the path is not empty, the node
  // sits beyond the forwarders that relayed the advert.
  uint8_t dist = (uint8_t)(hops + 1);

  // Time this advert, and take a clock reading from it, BEFORE the peer-table
  // rules below apply. The code gives a table slot only to a node within two
  // hops. That rule is correct for a neighbour table, and fatal here. A
  // repeater indoors hears mostly distant traffic. When the code gated the
  // reading on the slot, the estimator had no data at all.
  int32_t  clock_delta = 0;
  bool     have_clock = false;
  if (_clock != nullptr) {
    uint32_t ours = _clock->getCurrentTime();
    if (their_ts >= MIN_SANE_EPOCH) {
      noteSighting(pub, their_ts, hops);
      noteClockSample(pub, hops, their_ts);
      if (ours >= MIN_SANE_EPOCH) {                 // for the peer-table display only
        clock_delta = (int32_t)(their_ts - ours);
        have_clock = true;
      }
    }
  }

  int idx = findPeer(pub, 3);
  if (idx == -2) return;                              // ambiguous: change nothing
  if (idx < 0) {
    if (dist > 2) return;                             // only a near node earns a slot
    idx = claimSlot(millis());
    if (idx < 0) return;                              // the code can evict nothing
    PeerEntry& n = _peers[idx];
    memset(&n, 0, sizeof(n));
    memcpy(n.hash, pub, 3);
    n.width = 3;
  }
  PeerEntry& e = _peers[idx];
  if (e.min_hops == 0 || dist < e.min_hops) e.min_hops = dist;
  e.last_ms = millis();
  if (hops == 0) {                                    // we heard it on our own radio
    e.direct_rx++;
    e.last_direct_ms = e.last_ms;
    e.snr4_sum += snr4;
    e.snr_n++;
  }

  memcpy(e.pub, pub, sizeof(e.pub));

  // Their clock against ours, from the signed timestamp of this advert. Use
  // zero-hop adverts only. PeerEntry::clock_delta_s explains why the code
  // cannot use a relayed advert. Both nodes must think that they know the
  // date. If not, the subtraction measures "never synced" and not the drift.
  if (have_clock && hops == 0) {   // the peer table shows the skew for neighbours only
    e.clock_delta_s = clock_delta;
    e.clock_ms = e.last_ms;
    e.clock_n++;
  }

  /* app_data comes after [pub_key 32][timestamp 4][signature 64]. parseAdvert
     returned `pub`, which points at the start of that payload. Therefore this
     code recovers the offset that parseAdvert computed. It does not compute
     the offset a second time. */
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
      // This is a later copy of an advert that we have already seen. Only a
      // copy that travelled further counts. A path of equal or shorter length
      // is a different branch of the flood. It is not another hop of the same
      // branch.
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
  return d > 60000 ? 60000 : (uint16_t)d;      // one minute for each hop is already too much
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
    // This is the same node again. Keep the reading that the new one replaces,
    // so that the code can estimate a rate. Keep it only after the old reading
    // is far enough back in time to have a meaning.
    ClockReading& s = _clock_samples[idx];
    if (s.ms != 0 && (s.prev_ms == 0 ||
        (uint32_t)(s.ms - s.prev_ms) >= DRIFT_MIN_SPAN_MS)) {
      s.prev_their_ts = s.their_ts;
      s.prev_ms = s.ms;
    }
    // Use the shortest path that we have heard recently. A zero-hop reading
    // needs no correction. Therefore a relayed copy must not replace one.
    if (hops > s.hops && (uint32_t)(now - s.ms) < CLOCK_VOTE_MAX_AGE_MS / 4) return;
  }
  ClockReading& s = _clock_samples[idx];
  s.their_ts = their_ts;
  s.hops = hops;
  s.ms = now;
}

int MeshObserver::clockSamples(mesh::ClockSample* out, int max) const {
  if (out == nullptr || max <= 0 || _clock == nullptr) return 0;

  const uint16_t hop_ms = hopDelayMs();
  const uint32_t now = millis();
  const uint32_t ours = _clock->getCurrentTime();
  int n = 0;

  for (int i = 0; i < _num_clock_samples && n < max; i++) {
    const ClockReading& s = _clock_samples[i];
    if (s.ms == 0) continue;
    if ((uint32_t)(now - s.ms) > CLOCK_VOTE_MAX_AGE_MS) continue;   // too old
    if (s.hops > mesh::CLOCK_MAX_HOPS) continue;

    /* Age the reading forward by our own elapsed milliseconds, and subtract
       HERE. The ring holds what the peer said, never a difference from our
       clock, because a difference has a meaning only against the clock that
       measured it. A clock set does not touch millis(), so this stays correct
       across one. */
    uint32_t elapsed_s = (uint32_t)(now - s.ms) / 1000;
    uint32_t theirs_now = s.their_ts + elapsed_s;
    // A node whose own clock is not set cannot help us to set ours.
    if (theirs_now < mesh::CLOCK_SET_EPOCH) continue;

    /* A rate that no crystal can produce means that something SETS that clock.
       The clock does not drift, and its present value says nothing about the
       true time. The code measures their elapsed seconds against our elapsed
       milliseconds, so the measurement stays correct even if a person stepped
       our own clock between the two readings. */
    if (s.prev_ms != 0) {
      uint32_t span = (uint32_t)(s.ms - s.prev_ms);
      if (span >= DRIFT_MIN_SPAN_MS) {
        int64_t slip = (int64_t)(s.their_ts - s.prev_their_ts) * 1000LL - (int64_t)span;
        int64_t per_day = (slip * 86400000LL) / ((int64_t)span * 1000LL);
        if (per_day > MAX_SANE_DRIFT_S_PER_DAY ||
            per_day < -MAX_SANE_DRIFT_S_PER_DAY) continue;
      }
    }

    int32_t off = (int32_t)(theirs_now - ours);

    /* Collapse a group onto one reading, and carry its size as the weight.
       Nodes that share an upstream sync source share the exact error of that
       source. In the survey one group of 41 nodes held a single offset, and
       counted one by one it would have voted 41 times. The comparison is on
       the offset AFTER the hop correction, so a group that reached us by
       different path lengths still collapses. */
    int32_t corrected = off + mesh::clockHopCorrectionS(s.hops, hop_ms);
    bool dup = false;
    for (int j = 0; j < n; j++) {
      if (out[j].offset_s + mesh::clockHopCorrectionS(out[j].hops, hop_ms) != corrected) continue;
      if (out[j].weight < 255) out[j].weight++;
      // Keep the shortest path of the group, because it carries the most weight.
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
