#include "ObserverNeighbours.h"

void ObserverNeighbours::refresh(uint32_t now_ms) {
  _n = 0;
  const int total = _obs.numPeers();
  for (int i = 0; i < total && _n < MeshObserver::MAX_PEERS; i++) {
    const MeshObserver::PeerEntry* e = _obs.peer(i);
    if (e == NULL || e->direct_rx == 0) continue;
    /* Unsigned subtraction, so a millis() wrap gives a small difference rather
       than a huge one, and the neighbour survives the wrap instead of all of
       them vanishing at once every 49 days. */
    if ((uint32_t)(now_ms - e->last_direct_ms) > _stale_ms) continue;
    _idx[_n++] = (uint8_t)i;
  }

  // Insertion sort: at most 48 entries, already near-sorted between frames,
  // and it costs no scratch memory. SNR descending; a peer with no SNR sample
  // sorts last rather than pretending to be 0 dB.
  for (int i = 1; i < _n; i++) {
    uint8_t key = _idx[i];
    const MeshObserver::PeerEntry* ke = _obs.peer(key);
    int32_t ks = ke->snr_n ? meanSnr4(*ke) : INT32_MIN;
    int j = i - 1;
    while (j >= 0) {
      const MeshObserver::PeerEntry* je = _obs.peer(_idx[j]);
      int32_t js = je->snr_n ? meanSnr4(*je) : INT32_MIN;
      if (js >= ks) break;
      _idx[j + 1] = _idx[j];
      j--;
    }
    _idx[j + 1] = key;
  }
}

bool ObserverNeighbours::getNeighbour(int i, NeighbourRow& out) {
  if (i < 0 || i >= _n) return false;
  const MeshObserver::PeerEntry* e = _obs.peer(_idx[i]);
  if (e == NULL) return false;

  out.name = e->name;
  out.hash = e->hash;
  out.hash_len = e->width;
  out.has_snr = e->snr_n != 0;
  out.snr4 = meanSnr4(*e);
  out.rx = e->direct_rx;
  // heard_us only, never heard_us_1b: a single byte of hash matches by chance
  // often enough that counting it would inflate every row.
  out.fwd = e->heard_us;
  return true;
}
