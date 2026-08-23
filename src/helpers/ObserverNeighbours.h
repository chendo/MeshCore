#pragma once

#include "MeshObserver.h"
#include "ui/NeighboursScreen.h"

/**
 * \brief  Presents MeshObserver's peer table to NeighboursScreen.
 *
 * All of the display POLICY lives here, deliberately, so the screen stays a
 * dumb renderer and this stays host-testable without a framebuffer:
 *
 *  - DIRECT NEIGHBOURS ONLY. A peer qualifies when we have received one of its
 *    transmissions as the final hop (direct_rx > 0). Peers we only ever saw
 *    inside someone else's path are mesh topology, not neighbours.
 *  - STALENESS. A neighbour not heard directly for STALE_MS is dropped. A radio
 *    neighbour list that still lists last night's contacts is worse than a
 *    short one, because it reads as current.
 *  - ORDER. SNR descending. The strongest link is the one worth seeing first,
 *    and it puts a degrading link on the move down the screen.
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
};
