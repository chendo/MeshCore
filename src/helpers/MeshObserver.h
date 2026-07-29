#pragma once

// Passive observability for a MeshCore node: everything that can be learned by
// watching traffic go past, with no protocol participation of its own.
//
// This deliberately knows nothing about identities, radios or arbitration. It
// is fed raw frames and reports what it saw, so it works equally on a
// multi-identity board (where the shared-radio arbiter feeds it) and on a plain
// single-identity repeater (where a receive hook does). The peer table and the
// histograms below started life inside SharedRadioCore purely because that
// happened to be where every frame passed on one particular board — none of it
// is arbiter logic.
//
// PATHS ARE THE SOURCE OF TRUTH. Every forwarder appends its own hash to the
// END of a packet's path (Mesh::routeRecvPacket), which makes two independent
// facts recoverable — and because RF links are asymmetric, they genuinely
// differ:
//
//   LAST entry in a path   that node transmitted the frame WE received, so we
//                          can hear IT, and the frame's SNR/RSSI describe that
//                          link. A mid-path entry says nothing about its own
//                          link to us and must never be read that way.
//   entry right AFTER
//   one of ours            that node received OUR transmission and relayed it,
//                          so it can hear US.
//
// Hash width is chosen by the packet's originator rather than by us, so the
// same node turns up at one and two bytes. A 1-byte match is a 1-in-256
// coincidence and is never treated as proof: it is counted separately, and
// attributed to a known wider peer only when exactly one candidate exists.

#include <Arduino.h>
#include <MeshCore.h>

class MeshObserver {
public:
  static const int MAX_PEERS = 48;

  struct PeerEntry {
    uint8_t  hash[3];          // widest prefix seen
    uint8_t  width;            // 1..3 bytes known
    uint32_t direct_rx;        // seen as LAST hop: we received its transmission
    uint32_t relays;           // seen anywhere in a path: mesh activity only
    uint32_t heard_us;         // >=2-byte hash right after ours: CONFIRMED
    uint32_t heard_us_1b;      // same at 1 byte: provisional, never confirmation
    uint32_t last_ms;          // any sighting
    uint32_t last_direct_ms;   // last time it was the final hop
    int32_t  snr4_sum;         // running mean of SNR*4, direct sightings only
    uint32_t snr_n;
    uint8_t  min_hops;         // closest distance seen (1 = direct); 0 unknown
    // Identity, harvested from ADVERTs. A path only ever carries truncated
    // hashes, so a node stays anonymous until it adverts (or one of its adverts
    // reaches us) — at which point pubkey prefix, name and location can be
    // pinned to it and remembered.
    uint8_t  pub[6];           // pubkey prefix; all-zero while unknown
    int32_t  lat_e6, lon_e6;   // 0 when not advertised
    char     name[20];
  };

  // Register one of OUR public keys, so "did somebody relay us?" can be
  // answered and we never record ourselves as our own peer. Call once per
  // identity; a single-identity node calls it once.
  void addSelfKey(const uint8_t* pub_key);

  // Feed every received frame, with the SNR it arrived at (in quarter-dB, as
  // the packet log stores it).
  void observeRx(const uint8_t* frame, int len, int8_t snr4);

  // Feed every frame WE transmit. Only floods can come back to us relayed, so
  // only those are tracked. `stream` distinguishes identities on a shared radio;
  // a single-identity node leaves it at 0.
  void observeTx(const uint8_t* frame, int len, int stream = 0);

  // ---- relay confirmation ---------------------------------------------------
  // Proof that a transmission of ours was actually received by somebody: our
  // hash turns up in the path of a packet we later overhear, meaning a
  // neighbour took it and passed it on. This is the only direct evidence a node
  // gets that it is being heard at all — transmit counters only prove we keyed
  // the radio.
  //
  // Only 2-byte-or-wider hashes count. A 1-byte match collides once every 256
  // packets, which on a busy band is constant, so those are tallied separately
  // and never credited.
  static const int MAX_STREAMS = 8;
  uint32_t floodsSent(int stream = 0) const {
    return (stream >= 0 && stream < MAX_STREAMS) ? _flood_sent[stream] : 0;
  }
  uint32_t floodsConfirmed(int stream = 0) const {
    return (stream >= 0 && stream < MAX_STREAMS) ? _flood_confirmed[stream] : 0;
  }
  // how many confirmations arrived at each hash width (1..4 bytes)
  uint32_t confirmsByWidth(int bytes) const {
    return (bytes >= 1 && bytes <= 4) ? _confirm_width[bytes - 1] : 0;
  }

  // ---- what was learned ----
  int numPeers() const { return _num_peers; }
  const PeerEntry* peer(int i) const { return (i >= 0 && i < _num_peers) ? &_peers[i] : nullptr; }
  // peers that have demonstrably received one of our transmissions (2-byte+)
  int confirmedPeerCount() const;

  // How far away the traffic we hear originates. Bucket 0 = arrived with an
  // empty path, i.e. straight off the sender's radio. Distinguishes "we are on
  // the edge of a deep mesh" from "we have close neighbours".
  static const int HOP_BUCKETS = 16;
  uint32_t hopCount(int hops) const {
    return (hops >= 0 && hops < HOP_BUCKETS) ? _hops[hops] : 0;
  }
  // What KIND of traffic passes: index is the MeshCore payload type (0..15).
  uint32_t typeCount(int type) const {
    return (type >= 0 && type < 16) ? _types[type] : 0;
  }
  uint32_t framesObserved() const { return _frames; }

  void reset();

private:
  // Exactly one entry matching `hash` to min(width, entry width) bytes, or
  // -1 for none and -2 when the prefix is too short to disambiguate.
  int  findPeer(const uint8_t* hash, uint8_t width) const;
  void notePeersInPath(const uint8_t* frame, int len, int8_t snr4);
  void noteAdvert(const uint8_t* frame, int len, int8_t snr4);
  int  selfIndex(const uint8_t* hash, uint8_t width) const;   // -1 if not ours
  bool isSelf(const uint8_t* hash, uint8_t width) const { return selfIndex(hash, width) >= 0; }

  PeerEntry _peers[MAX_PEERS];
  int       _num_peers = 0;

  static const int MAX_SELF = 8;
  uint8_t _self[MAX_SELF][4];
  int     _num_self = 0;

  // Recent flood transmits awaiting confirmation. A relay may take a while to
  // come back, so this is a time window rather than a single slot.
  static const int TX_RING = 16;
  static const uint32_t CONFIRM_WINDOW_MS = 30000;
  struct TxRecord { uint32_t t_ms; int8_t stream; bool confirmed; };
  TxRecord _tx_ring[TX_RING];
  uint8_t  _tx_ring_head = 0, _tx_ring_count = 0;
  uint32_t _flood_sent[MAX_STREAMS] = {0};
  uint32_t _flood_confirmed[MAX_STREAMS] = {0};
  uint32_t _confirm_width[4] = {0};
  // credit the most recent unconfirmed flood from `stream`, if one is in window
  void creditRelay(int stream, uint8_t hash_width);

  uint32_t _hops[HOP_BUCKETS] = {0};
  uint32_t _types[16] = {0};
  uint32_t _frames = 0;
};
