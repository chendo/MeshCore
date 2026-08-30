#pragma once

#include "MeshObserver.h"
#include "ui/NeighboursScreen.h"

/**
 * \brief  Presents MeshObserver's peer table to NeighboursScreen.
 *
 * All of the display POLICY lives here, deliberately, so the screen stays a
 * dumb renderer and this stays host-testable without a framebuffer:
 *
 *  - EVERY PEER, not just the ones we hear directly. A node three hops out is
 *    still part of the picture this node has of the mesh.
 *  - STALENESS. A peer not seen at all for STALE_MS is dropped. A list that
 *    still shows last night's contacts is worse than a short one, because it
 *    reads as current.
 *  - ORDER. Closest first: min_hops ascending, and within one hop count, SNR
 *    descending. min_hops is 1 when we hear the node's own radio and 0 when we
 *    have never placed it, so unknown distance sorts to the BOTTOM rather than
 *    pretending to be nearer than everything else. A peer with no SNR sample
 *    sorts last inside its own hop group, for the same reason.
 */
class ObserverNeighbours : public NeighbourSource {
public:
  // Six hours. Long enough to survive a quiet afternoon on a sparse mesh,
  // short enough that anything listed was reachable within the same day.
  static const uint32_t STALE_MS = 6UL * 60 * 60 * 1000;

  ObserverNeighbours(const MeshObserver& obs, uint32_t stale_ms = STALE_MS)
    : _obs(obs), _stale_ms(stale_ms), _n(0) { }

  /** Rebuild the sorted index. Call once per frame, before rendering: the
   *  observer's table moves underneath us, and a comparator reading a table
   *  that changes mid-sort is how you get a garbled list. */
  void refresh(uint32_t now_ms);

  int numNeighbours() override { return _n; }
  bool getNeighbour(int i, NeighbourRow& out) override;

private:
  const MeshObserver& _obs;
  uint32_t _stale_ms;
  uint8_t _idx[MeshObserver::MAX_PEERS];
  int _n;

  static int32_t meanSnr4(const MeshObserver::PeerEntry& e) {
    return e.snr_n ? e.snr4_sum / (int32_t)e.snr_n : 0;
  }

  // 0 means "never placed", which must sort last, not first.
  static uint8_t hopRank(const MeshObserver::PeerEntry& e) {
    return e.min_hops == 0 ? 255 : e.min_hops;
  }

  // Should a come before b? Closest first, then strongest first.
  static bool before(const MeshObserver::PeerEntry& a, const MeshObserver::PeerEntry& b) {
    uint8_t ha = hopRank(a), hb = hopRank(b);
    if (ha != hb) return ha < hb;
    if (a.snr_n == 0 || b.snr_n == 0) return a.snr_n != 0;   // sampled beats unsampled
    return meanSnr4(a) > meanSnr4(b);
  }
};
