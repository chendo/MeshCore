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
    // Clock skew. An ADVERT is signed over a timestamp its originator chose,
    // so an advert heard at ZERO HOPS is a direct reading of that node's clock
    // against ours — the only time reference the mesh hands us for free.
    //
    // Zero hops is not a nicety. A relayed advert still carries the moment it
    // was created, and a flood takes seconds to minutes to work across the
    // mesh (and is re-flooded long after), so the same subtraction on a
    // multi-hop advert measures propagation delay, not skew, and always makes
    // the far node look slow. Those are discarded rather than averaged in.
    int32_t  clock_delta_s;    // their clock minus ours; positive => they are ahead
    uint32_t clock_ms;         // millis() at that reading; 0 = never measured
    uint32_t clock_n;          // readings taken

  };

  // Below this a clock is simply unset rather than wrong: a node that has
  // never been disciplined reports something near zero or its build epoch, and
  // recording a 56-year "skew" for it would say nothing about anyone's drift.
  static const uint32_t MIN_SANE_EPOCH = 1700000000UL;   // 2023-11-14

  /* A clock reading below this is not a wrong time, it is NO time: these boards
     have no hardware RTC, so every reboot drops them back to VolatileRTCClock's
     built-in 15 May 2024 and they stay there until something tells them
     otherwise. 1 Jan 2025 sits safely above that default and below any real
     deployment, so it separates "never set" from "set and drifting" without
     needing to know when the firmware was built.
     This cuts both ways: a node whose own clock is below it must not vote, and
     one such neighbour is already out there on this mesh reading +71038395s. */
  static const uint32_t CLOCK_SET_EPOCH = 1735689600UL;  // 2025-01-01
  static const uint16_t HOP_DELAY_DEFAULT_MS = 1500;
  /* Below this many measured pairs the mean is too noisy to beat the computed
     default, since each pair carries the full spread of one random relay wait. */
  static const uint32_t HOP_DELAY_MIN_PAIRS = 8;

  /* Clock consensus -- see clockConsensus(). Thresholds come from a 407-node
     survey of a real regional mesh:
       - median offset -12s, MAD 7s, but the mean was -46s and the range
         -3533..+1239, so any mean-based estimate is unusable;
       - 21% of nodes were outliers by 3*MAD, and discarding them left the
         estimate unchanged;
       - the extremes were not lone bad clocks but whole GROUPS sharing an
         offset (-227s x6, -211s x5, +335s x4), i.e. sub-networks that agreed
         with each other and were wrong together;
       - 10 nodes reported physically impossible rates (up to 58485 s/day). */
  static const int32_t  MAX_SANE_DRIFT_S_PER_DAY = 50;
  static const uint32_t CLOCK_VOTE_MAX_AGE_MS = 60UL * 60UL * 1000UL;   // 1 hour
  static const uint8_t  CLOCK_MIN_SOURCES = 3;

  /* Clock samples live in their own ring rather than in the peer table. A peer
     slot is only granted to a node within two hops (see noteAdvert), which is
     the right rule for a NEIGHBOUR table and the wrong one here: a repeater
     indoors may hear one zero-hop advert in fifty frames, and refusing the
     other forty-nine leaves the estimator starved. The ring is also a better
     shape for the job -- what matters is a recent spread of readings, not
     per-node history. */
  static const uint8_t  CLOCK_SAMPLES = 24;
  /* Beyond this the accumulated propagation correction, and the uncertainty in
     the constant used to make it, dominate whatever the reading is worth. */
  static const uint8_t  MAX_CLOCK_HOPS = 8;

  /* Stores what the OTHER node said, never the difference from our own clock.
     A difference is only meaningful against the clock it was measured with, so
     the moment ours is stepped -- by a person, or by convergence itself -- every
     stored difference silently becomes a lie. Keeping the absolute timestamp
     and subtracting at the point of use makes the estimate immune to that: it
     is recomputed against whatever our clock reads now. millis() is unaffected
     by clock sets, so the elapsed-time correction stays valid across them too. */
  struct ClockSample {
    uint8_t  pub4[4];
    uint32_t their_ts;       // their clock's reading when the advert was stamped
    uint32_t ms;             // millis() when we heard it
    uint32_t prev_their_ts;  // the reading this one replaced, for a drift estimate
    uint32_t prev_ms;
    uint8_t  hops;           // 0 = straight off their radio
  };
  /* Both clocks are read to the second, so a drift verdict taken over a short
     span is mostly quantisation: to call 50 s/day apart from noise the readings
     must be hours apart, not minutes. Below this span we abstain rather than
     reject -- a racing clock will be an outlier soon enough anyway. */
  static const uint32_t DRIFT_MIN_SPAN_MS = 2UL * 60UL * 60UL * 1000UL;
  /* Floor under the outlier threshold, so a mesh that already agrees to within
     a second does not reject almost everything for being 1s out. */
  static const int32_t  CLOCK_CLIP_FLOOR_S = 2;
  /* Half-width of the band a "consensus" has to fit inside. Deliberately much
     tighter than the errors being rejected: a mesh whose honest members really
     do disagree by more than a couple of minutes has no consensus worth acting
     on, and picking a side of that would be guessing. */
  static const int32_t  CLOCK_CLUSTER_WIDTH_S = 120;

  struct ClockConsensus {
    bool     valid;
    int32_t  offset_s;    // seconds to ADD to our clock to join the consensus
    uint8_t  n_seen;      // peers that offered a usable reading
    uint8_t  n_used;      // survivors after outlier rejection
    uint8_t  agree_pct;   // n_used * 100 / n_seen -- what fraction survived
    uint8_t  n_zero_hop;  // how many survivors were heard directly (reporting only)
    uint16_t hop_delay_ms;// the per-hop correction actually applied
    /* Spread of the survivors. agree_pct alone is NOT a confidence measure: a
       population split evenly between two beliefs 300s apart loses nobody to
       clipping, so it reports 100% agreement on a median that not one node
       actually holds. Anything about to act on offset_s must check this too. */
    int32_t  spread_s;
  };

  /**
   * @brief  What the neighbourhood thinks our clock error is.
   *
   * Median of per-peer offsets after discarding outliers by median-absolute-
   * deviation. Readings are zero-hop by construction (a relayed advert measures
   * propagation delay, not skew), and are additionally filtered by age, by
   * whether the peer's apparent rate is physically possible, and by collapsing
   * peers reporting an identical offset -- in the survey a single group of 41
   * nodes shared one offset, and left uncollapsed it would have voted 41 times.
   *
   * Reports only. Deciding whether to act on it is the caller's business.
   */
  ClockConsensus clockConsensus(uint8_t min_sources = CLOCK_MIN_SOURCES) const;

  /**
   * @brief  Measured one-way propagation delay per relay hop, in milliseconds.
   *
   * Not a clock comparison: because this class is fed raw frames BEFORE the
   * mesh dedups them, it sees the originator's own transmission and then the
   * relayed copies of that same advert. The gap between those arrivals, over
   * the hop difference, is the delay itself, measured against our own millis().
   *
   * Falls back to HOP_DELAY_DEFAULT_MS until enough pairs have been seen.
   * For the record, the arithmetic that default comes from: a repeater waits
   * rng(0, 5*airtime*tx_delay_factor) before relaying and then spends airtime
   * transmitting, so at the repeater default factor of 0.5 the mean is
   * 2.25*airtime -- about 1.5s for a 130-byte advert at SF7/BW62.5.
   */
  uint16_t hopDelayMs() const;
  uint32_t hopDelayPairs() const { return _hop_delay_pairs; }
  int numClockSamples() const { return _num_clock_samples; }

  // Register one of OUR public keys, so "did somebody relay us?" can be
  // answered and we never record ourselves as our own peer. Call once per
  // identity; a single-identity node calls it once.
  void addSelfKey(const uint8_t* pub_key);

  // Our own time source, so peers' advert timestamps can be differenced
  // against something. Optional: leave it unset and clock skew is simply never
  // recorded — nothing else in the observer depends on it.
  void setClock(mesh::RTCClock* clk) { _clock = clk; }

  // Feed every received frame, with the SNR it arrived at (in quarter-dB, as
  // the packet log stores it).
  void observeRx(const uint8_t* frame, int len, int8_t snr4);

  // Feed an advert that arrived over a BRIDGE rather than the radio.
  //
  // Clock samples only. Deliberately does NOT touch the peer table, the hop or
  // type histograms, the frame counter or any RSSI/SNR statistic: every one of
  // those answers "what can this node hear", and a bridged packet was heard by
  // a node on a different band. Counting it would put nodes in our neighbour
  // table that our radio has never received a single symbol from.
  //
  // The timestamp inside it is unaffected by any of that. A bridge peer is
  // authenticated (HMAC over the whole frame), so an advert relayed by one is
  // if anything better evidence than a promiscuously overheard one -- and on a
  // node alone on its band it is the ONLY evidence available. Without this a
  // bridge node can never set its clock at all.
  void observeBridgedAdvert(const uint8_t* frame, int len);

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

  // How recently we must have transmitted for a returning echo to be credited
  // to it. Our hash sits in the path of EVERY packet we ever forwarded, so a
  // wide window credits whichever transmit happens to be newest rather than the
  // one that actually came back — with a busy repeater forwarding continuously,
  // that is close to guesswork.
  //
  // The protocol sets the floor: a relay waits nextInt(0, 5*airtime*factor)
  // before retransmitting (Mesh::getRetransmitDelay and the repeater's
  // override), which at SF7/62.5kHz on a ~130-byte frame is up to about 2s for
  // a single hop. 5s covers that with margin and is tight enough that the
  // correlation means something.
  static const uint32_t CONFIRM_WINDOW_DEFAULT_MS = 5000;
  void setConfirmWindow(uint32_t ms) { _confirm_window_ms = ms; }
  uint32_t confirmWindow() const { return _confirm_window_ms; }

  // ---- table pressure -------------------------------------------------------
  // The table is small and the mesh is not, so once every slot is taken it has
  // to choose what to forget. Plain LRU is wrong here: it would discard a
  // neighbour we have PROVEN can hear us in favour of a node we glimpsed once in
  // somebody else's path. Value decides first; recency only breaks ties.
  //
  //   3  heard_us > 0            it relayed something of OURS — two-way, proven
  //   2  direct + strong SNR     we hear it well, straight off its radio
  //   1  direct, marginal SNR    we hear it, but the link is weak
  //   0  relay sightings only    never heard directly; may not even be nearby
  //
  // New entries start at tier 0, which makes the bottom of the table probation:
  // an arrival has to earn a direct sighting before it becomes hard to displace.
  //
  // Anything at tier 1 or above is evicted ONLY once it has gone quiet for
  // STALE_MS. Without that, a table full of good neighbours would be steadily
  // cannibalised by unproven relay sightings, which is the opposite of useful.
  // When nothing is eligible the new sighting is refused and counted, so a
  // wedged table is visible as refusals rather than looking like a quiet mesh.
  static const int8_t   STRONG_SNR_4 = 0;             // mean SNR*4 >= 0 dB
  static const uint32_t STALE_MS = 3600000UL;         // an hour with no sighting
  int peerTier(const PeerEntry& e) const;
  uint32_t evictions() const { return _evictions; }
  uint32_t refusedInserts() const { return _refused; }
  // entries currently held at each hash width; 1-byte ones are collision-prone
  // and should not be presented as confidently as the rest
  int widthCount(int bytes) const;

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
  // Slot for a newly-seen node: a free one, else an evictable entry, else -1
  // (refused). Ages are computed against `now` so the millis() wrap is harmless.
  int  claimSlot(uint32_t now);
  int  evictionVictim(uint32_t now) const;
  void notePeersInPath(const uint8_t* frame, int len, int8_t snr4);
  void noteAdvert(const uint8_t* frame, int len, int8_t snr4);
  /* Shared advert parse: true if the frame is a well-formed advert from
     somebody other than us, yielding the originator's key, its hop count and
     the timestamp it claimed. The timestamp is NOT range-checked here --
     callers decide, because the peer table still wants an advert whose clock
     is nonsense while the clock estimator must reject it. Used by both the
     radio path and the bridge path so the two cannot drift apart on framing. */
  bool parseAdvert(const uint8_t* frame, int len,
                   const uint8_t*& pub, uint8_t& hops, uint32_t& their_ts) const;
  int  selfIndex(const uint8_t* hash, uint8_t width) const;   // -1 if not ours
  bool isSelf(const uint8_t* hash, uint8_t width) const { return selfIndex(hash, width) >= 0; }

  PeerEntry _peers[MAX_PEERS];

  ClockSample _clock_samples[CLOCK_SAMPLES];
  uint8_t     _num_clock_samples = 0;
  void noteClockSample(const uint8_t* pub, uint8_t hops, uint32_t their_ts);

  /* One advert in flight, so later copies of it can be timed against the first.
     Keyed by originator and advert timestamp, which together identify an advert
     independently of the path it arrived by. */
  struct AdvertSighting {
    uint8_t  pub4[4];
    uint32_t advert_ts;
    uint32_t last_ms;
    uint8_t  last_hops;
  };
  static const uint8_t ADVERT_SIGHTINGS = 16;
  AdvertSighting _sightings[ADVERT_SIGHTINGS];
  uint8_t  _num_sightings = 0;
  uint32_t _hop_delay_sum_ms = 0;
  uint32_t _hop_delay_hops = 0;
  uint32_t _hop_delay_pairs = 0;
  void noteSighting(const uint8_t* pub, uint32_t advert_ts, uint8_t hops);
  int       _num_peers = 0;
  uint32_t  _evictions = 0;
  uint32_t  _refused = 0;

  static const int MAX_SELF = 8;
  uint8_t _self[MAX_SELF][4];
  int     _num_self = 0;

  mesh::RTCClock* _clock = nullptr;

  // Recent flood transmits awaiting confirmation. A relay may take a while to
  // come back, so this is a time window rather than a single slot.
  static const int TX_RING = 16;
  uint32_t _confirm_window_ms = CONFIRM_WINDOW_DEFAULT_MS;
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
