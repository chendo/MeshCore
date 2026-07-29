#include "MeshObserver.h"

void MeshObserver::addSelfKey(const uint8_t* pub_key) {
  if (pub_key == nullptr || _num_self >= MAX_SELF) return;
  memcpy(_self[_num_self++], pub_key, 4);
}

void MeshObserver::reset() {
  _num_peers = 0;
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
      if (_num_peers >= MAX_PEERS) continue;     // table full; keep what we have
      idx = _num_peers++;
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

void MeshObserver::noteAdvert(const uint8_t* frame, int len, int8_t snr4) {
  if (((frame[0] >> 2) & 0x0F) != 4) return;          // PAYLOAD_TYPE_ADVERT

  uint8_t route = frame[0] & 0x03;
  int o = 1;
  if (route == 0 || route == 3) o += 4;
  if (o >= len) return;
  uint8_t pl = frame[o++];
  uint8_t hops = pl & 63;
  uint8_t sz = (pl >> 6) + 1;
  if (sz > 3 || o + hops * sz > len) return;
  o += hops * sz;

  // payload: [pub_key 32][timestamp 4][signature 64][app_data]
  if (o + 100 > len) return;
  const uint8_t* pub = &frame[o];
  if (isSelf(pub, 4)) return;                         // never record ourselves

  // An empty path means we received the originator's own transmission, so it is
  // one hop away. Otherwise it sits beyond the forwarders that did relay it.
  uint8_t dist = (uint8_t)(hops + 1);

  int idx = findPeer(pub, 3);
  if (idx == -2) return;                              // ambiguous, leave alone
  if (idx < 0) {
    if (dist > 2 || _num_peers >= MAX_PEERS) return;  // only near nodes earn a slot
    idx = _num_peers++;
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

  const uint8_t* ad = &frame[o + 100];
  int ad_len = len - (o + 100);
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
