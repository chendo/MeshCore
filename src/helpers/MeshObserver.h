#pragma once

// Passive observability for a MeshCore node. It reports everything that the
// node can learn while it watches traffic pass. It takes no part in the
// protocol itself.
//
// This class knows nothing about identities, radios or arbitration. The caller
// gives it raw frames, and it reports what it saw. Therefore it works on a
// multi-identity board, where the shared-radio arbiter supplies the frames, and
// it works equally well on a plain single-identity repeater, where a receive
// hook supplies them. The peer table and the histograms below started inside
// SharedRadioCore, only because that was where every frame passed on one
// particular board. None of it is arbiter logic.
//
// PATHS ARE THE SOURCE OF TRUTH. Every forwarder adds its own hash to the END
// of the path of a packet (Mesh::routeRecvPacket). Therefore we can recover two
// independent facts. RF links are asymmetric, so these two facts are truly
// different:
//
//   the LAST entry in a    That node transmitted the frame that WE received.
//   path                   Therefore we can hear IT, and the SNR and RSSI of
//                          the frame describe that link. An entry in the middle
//                          of a path says nothing about its own link to us. You
//                          must never read it that way.
//   the entry directly
//   AFTER one of ours      That node received OUR transmission and relayed it.
//                          Therefore it can hear US.
//
// The originator of the packet selects the hash width, not us. Therefore the
// same node appears at one byte and at two bytes. A 1-byte match is a 1-in-256
// coincidence, and we never use it as proof. The code counts it separately. It
// attributes such a match to a known wider peer only when exactly one candidate
// exists.

#include <Arduino.h>
#include <MeshCore.h>
#include "ClockPolicy.h"

class MeshObserver {
public:
  static const int MAX_PEERS = 48;

  struct PeerEntry {
    uint8_t  hash[3];          // the widest prefix seen
    uint8_t  width;            // 1..3 bytes known
    uint32_t direct_rx;        // seen as the LAST hop: we received its transmission
    uint32_t relays;           // seen at any place in a path: mesh activity only
    uint32_t heard_us;         // a hash of 2 bytes or more directly after ours: CONFIRMED
    uint32_t heard_us_1b;      // the same at 1 byte: provisional, never a confirmation
    uint32_t last_ms;          // any sighting
    uint32_t last_direct_ms;   // the last time it was the final hop
    int32_t  snr4_sum;         // the running mean of SNR*4, from direct sightings only
    uint32_t snr_n;
    uint8_t  min_hops;         // the shortest distance seen (1 = direct); 0 = unknown
    // The identity, collected from ADVERTs. A path carries only truncated
    // hashes. Therefore a node stays anonymous until it adverts, or until one
    // of its adverts reaches us. At that point the code can attach a pubkey
    // prefix, a name and a location to it, and hold them.
    uint8_t  pub[6];           // the pubkey prefix; all zero while unknown
    int32_t  lat_e6, lon_e6;   // 0 when the node did not advertise them
    char     name[20];
    // The clock skew. The originator of an ADVERT signs it over a timestamp
    // that the originator chose. Therefore an advert heard at ZERO HOPS is a
    // direct reading of the clock of that node against ours. It is the only
    // free time reference that the mesh gives us.
    //
    // Zero hops is a requirement, not a preference. A relayed advert still
    // carries the moment when the node created it. A flood needs seconds to
    // minutes to cross the mesh, and nodes flood it again long after that.
    // Therefore the same subtraction on a multi-hop advert measures the
    // propagation delay, not the skew, and it always makes the distant node
    // look slow. The code discards those readings. It does not average them in.
    int32_t  clock_delta_s;    // their clock minus ours; a positive value means they are ahead
    uint32_t clock_ms;         // millis() at that reading; 0 = never measured
    uint32_t clock_n;          // the number of readings taken

  };

  // A clock below this value is not wrong. It is simply not set. A node that
  // nobody has disciplined reports a value near zero, or its build epoch. A
  // recorded "skew" of 56 years for that node says nothing about the drift of
  // any node.
  static const uint32_t MIN_SANE_EPOCH = 1700000000UL;   // 2023-11-14

  /* mesh::CLOCK_SET_EPOCH separates "never set" from "set and drifting", and
     ClockPolicy.h explains the value. It works in both directions: a node whose
     own clock is below it must not vote. One such neighbour is already on this
     mesh, and it reads +71038395s. */
  static const uint16_t HOP_DELAY_DEFAULT_MS = mesh::CLOCK_HOP_DELAY_DEFAULT_MS;
  /* Below this number of measured pairs, the mean has too much noise to beat
     the computed default. Each pair carries the full spread of one random
     relay wait. */
  static const uint32_t HOP_DELAY_MIN_PAIRS = 8;

  /* The largest rate that a crystal can produce. Ten nodes in the 407-node
     survey reported rates above it, the largest at 58485 s/day. A rate that
     high means that something SETS that clock, so the clock does not drift and
     its present value says nothing about the true time. */
  static const int32_t  MAX_SANE_DRIFT_S_PER_DAY = 50;
  static const uint32_t CLOCK_VOTE_MAX_AGE_MS = 60UL * 60UL * 1000UL;   // 1 hour

  /* The clock samples use their own ring, not the peer table. The code gives a
     peer slot only to a node within two hops (see noteAdvert). That is the
     correct rule for a NEIGHBOUR table, and the wrong rule here. A repeater
     indoors can hear one zero-hop advert in fifty frames. If the code refuses
     the other forty-nine, the estimator has too little data. The ring also has
     a better shape for this work. What matters is a recent spread of readings,
     not the history of each node. */
  static const uint8_t  CLOCK_SAMPLES = 24;
  /* This structure stores what the OTHER node said. It never stores the
     difference from our own clock. A difference has a meaning only against the
     clock that measured it. Therefore, at the moment a person steps our clock,
     or convergence steps it, every stored difference becomes wrong, and
     nothing reports this. The code instead keeps the absolute timestamp and
     subtracts at the point of use. The estimate is then immune to a step,
     because the code computes it again against the present reading of our
     clock. A clock set does not change millis(). Therefore the elapsed-time
     correction also stays valid across a clock set. */
  struct ClockReading {
    uint8_t  pub4[4];
    uint32_t their_ts;       // the reading of their clock when the advert was stamped
    uint32_t ms;             // millis() when we heard it
    uint32_t prev_their_ts;  // the reading that this one replaced, for a drift estimate
    uint32_t prev_ms;
    uint8_t  hops;           // 0 = directly off their radio
  };
  /* Both clocks read to the second. Therefore a drift verdict over a short
     span is mostly quantisation. To separate 50 s/day from the noise, the two
     readings must be hours apart, not minutes. Below this span the code
     abstains. It does not reject the sample, because a clock that runs fast
     becomes an outlier soon enough. */
  static const uint32_t DRIFT_MIN_SPAN_MS = 2UL * 60UL * 60UL * 1000UL;

  /**
   * @brief  Hand the present readings to the clock estimator.
   *
   * This is the boundary between the part that needs hardware and the part that
   * does not. Everything that needs the ring, millis() or our own clock happens
   * here. The function filters the readings by age, by whether the clock of the
   * peer is set at all, and by whether the apparent rate of that peer is
   * physically possible. It then ages each reading forward and subtracts it
   * from our clock AT THIS MOMENT. Peers that report an identical corrected
   * offset collapse onto one sample that carries the size of the group as its
   * weight, because in the survey one group of 41 nodes held a single offset,
   * and counted one by one that group would have voted 41 times.
   *
   * The statistics then happen in mesh::clockEstimate, which needs none of the
   * above and which a host test covers. See helpers/ClockPolicy.h.
   *
   * \param  out  an array of at least `max` samples
   * \param  max  how many samples the array holds
   * \returns  how many samples the function wrote
   */
  int clockSamples(mesh::ClockSample* out, int max) const;

  /**
   * @brief  The newest timestamp that this node has put on the air.
   *
   * Returns 0 while the node has sent nothing. This is the REPLAY FLOOR. A peer
   * drops an advert whose timestamp is not newer than the mark that the peer
   * already holds for us, so a clock that goes below this value takes the node
   * off the air for everybody who already knows it. mesh::clockDecide refuses
   * to cross it.
   */
  uint32_t sentHighWater() const { return _sent_high_s; }

  /**
   * @brief  The measured one-way propagation delay per relay hop, in
   *         milliseconds.
   *
   * This is not a clock comparison. The caller gives this class raw frames
   * BEFORE the mesh removes the duplicates. Therefore the class sees the
   * transmission of the originator, and then the relayed copies of that same
   * advert. The time between those arrivals, divided by the hop difference, is
   * the delay itself. The code measures it against our own millis().
   *
   * The function returns HOP_DELAY_DEFAULT_MS until it has seen enough pairs.
   * This is the arithmetic behind that default. A repeater waits
   * rng(0, 5*airtime*tx_delay_factor) before it relays, and then it spends the
   * airtime on the transmission. At the repeater default factor of 0.5, the
   * mean is 2.25*airtime. That is approximately 1.5s for a 130-byte advert at
   * SF7/BW62.5.
   */
  uint16_t hopDelayMs() const;
  uint32_t hopDelayPairs() const { return _hop_delay_pairs; }
  int numClockSamples() const { return _num_clock_samples; }

  // Register one of OUR public keys. The observer can then answer the question
  // "did a node relay us?", and it never records us as our own peer. Call this
  // one time for each identity. A single-identity node calls it one time.
  void addSelfKey(const uint8_t* pub_key);

  // Our own time source. The code can then subtract the advert timestamps of
  // the peers from it. This is optional. If you leave it unset, the code
  // records no clock skew. Nothing else in the observer needs it.
  void setClock(mesh::RTCClock* clk) { _clock = clk; }

  // Give the observer every received frame, together with the SNR that the
  // frame arrived at. The SNR is in quarter-dB, as the packet log stores it.
  void observeRx(const uint8_t* frame, int len, int8_t snr4);

  // Give the observer an advert that arrived over a BRIDGE, not over the radio.
  //
  // This function takes clock samples only. It does NOT change the peer table,
  // the hop histogram, the type histogram, the frame counter or any RSSI or
  // SNR statistic. Each of those answers the question "what can this node
  // hear", and a node on a different band heard the bridged packet. If the
  // code counted it, our neighbour table would hold nodes from which our radio
  // has never received one symbol.
  //
  // None of that changes the timestamp inside the advert. A bridge peer is
  // authenticated with an HMAC over the whole frame. Therefore an advert that
  // a bridge peer relays is at least as good as evidence as an advert that we
  // overhear. On a node that is alone on its band, it is the ONLY evidence
  // available. Without this function, a bridge node can never set its clock.
  void observeBridgedAdvert(const uint8_t* frame, int len);

  // Give the observer every frame that WE transmit. Only floods can return to
  // us as relayed copies, so the code tracks only those. `stream` separates
  // the identities on a shared radio. A single-identity node leaves it at 0.
  void observeTx(const uint8_t* frame, int len, int stream = 0);

  // ---- relay confirmation ---------------------------------------------------
  // This is proof that another node received one of our transmissions. Our
  // hash appears in the path of a packet that we overhear later. That means a
  // neighbour took our transmission and passed it on. It is the only direct
  // evidence a node gets that other nodes hear it. The transmit counters only
  // prove that we keyed the radio.
  //
  // Only hashes of 2 bytes or more count. A 1-byte match collides one time in
  // every 256 packets, which on a busy band is continuous. The code counts
  // those matches separately and never credits them.
  static const int MAX_STREAMS = 8;
  uint32_t floodsSent(int stream = 0) const {
    return (stream >= 0 && stream < MAX_STREAMS) ? _flood_sent[stream] : 0;
  }
  uint32_t floodsConfirmed(int stream = 0) const {
    return (stream >= 0 && stream < MAX_STREAMS) ? _flood_confirmed[stream] : 0;
  }
  // the number of confirmations that arrived at each hash width (1..4 bytes)
  uint32_t confirmsByWidth(int bytes) const {
    return (bytes >= 1 && bytes <= 4) ? _confirm_width[bytes - 1] : 0;
  }

  // How recent a transmission must be before the code credits a returning echo
  // to it. Our hash is in the path of EVERY packet that we have forwarded.
  // Therefore a large window credits the newest transmission, and not the
  // transmission that truly returned. On a busy repeater that forwards
  // continuously, such a credit is almost a guess.
  //
  // The protocol sets the minimum value. A relay waits
  // nextInt(0, 5*airtime*factor) before it transmits again. See
  // Mesh::getRetransmitDelay and the override in the repeater. At SF7/62.5kHz
  // on a frame of approximately 130 bytes, that wait is up to about 2s for one
  // hop. A window of 5s covers that wait with margin. It is also small enough
  // that the correlation has a meaning.
  static const uint32_t CONFIRM_WINDOW_DEFAULT_MS = 5000;
  void setConfirmWindow(uint32_t ms) { _confirm_window_ms = ms; }
  uint32_t confirmWindow() const { return _confirm_window_ms; }

  // ---- table pressure -------------------------------------------------------
  // The table is small and the mesh is not. Therefore, after every slot is
  // full, the code must select what to discard. Plain LRU is wrong here. It
  // would discard a neighbour that we have PROVEN can hear us, and keep a node
  // that we saw one time in the path of another node. The value decides first.
  // Recency only separates entries of equal value.
  //
  //   3  heard_us > 0            it relayed a packet of OURS: two-way, proven
  //   2  direct + strong SNR     we hear it well, directly off its radio
  //   1  direct, marginal SNR    we hear it, but the link is weak
  //   0  relay sightings only    we never heard it directly. It can be distant.
  //
  // A new entry starts at tier 0. The bottom of the table is therefore a
  // probation area. An arrival must earn a direct sighting before it becomes
  // difficult to displace.
  //
  // The code evicts an entry at tier 1 or above ONLY after that entry has been
  // silent for STALE_MS. Without that rule, unproven relay sightings would
  // slowly remove a table of good neighbours, which is the opposite of what we
  // want. When no entry qualifies, the code refuses the new sighting and
  // counts the refusal. A full table is then visible as refusals. It does not
  // look like a quiet mesh.
  static const int8_t   STRONG_SNR_4 = 0;             // mean SNR*4 >= 0 dB
  static const uint32_t STALE_MS = 3600000UL;         // one hour with no sighting
  int peerTier(const PeerEntry& e) const;
  uint32_t evictions() const { return _evictions; }
  uint32_t refusedInserts() const { return _refused; }
  // The entries that the table holds at each hash width. The 1-byte entries
  // collide easily. Do not present them with the same confidence as the rest.
  int widthCount(int bytes) const;

  // ---- what was learned ----
  int numPeers() const { return _num_peers; }
  const PeerEntry* peer(int i) const { return (i >= 0 && i < _num_peers) ? &_peers[i] : nullptr; }
  // the peers that gave proof that they received one of our transmissions
  // (at 2 bytes or more)
  int confirmedPeerCount() const;

  // The distance to the origin of the traffic that we hear. Bucket 0 means the
  // frame arrived with an empty path, that is, directly off the radio of the
  // sender. This separates "we are on the edge of a deep mesh" from "we have
  // close neighbours".
  static const int HOP_BUCKETS = 16;
  uint32_t hopCount(int hops) const {
    return (hops >= 0 && hops < HOP_BUCKETS) ? _hops[hops] : 0;
  }
  // Which KIND of traffic passes. The index is the MeshCore payload type
  // (0..15).
  uint32_t typeCount(int type) const {
    return (type >= 0 && type < 16) ? _types[type] : 0;
  }
  uint32_t framesObserved() const { return _frames; }

  void reset();

private:
  // Returns the one entry that matches `hash` over min(width, entry width)
  // bytes. Returns -1 when no entry matches. Returns -2 when the prefix is too
  // short to select one entry.
  int  findPeer(const uint8_t* hash, uint8_t width) const;
  // Returns a slot for a node that the code has just seen. It returns a free
  // slot, or an entry that the code may evict, or -1 for a refusal. The code
  // computes the ages against `now`, so a millis() wrap does no damage.
  int  claimSlot(uint32_t now);
  int  evictionVictim(uint32_t now) const;
  void notePeersInPath(const uint8_t* frame, int len, int8_t snr4);
  void noteAdvert(const uint8_t* frame, int len, int8_t snr4);
  /* The shared advert parser. It returns true if the frame is a correctly
     formed advert from a node other than us. It then supplies the key of the
     originator, the hop count and the timestamp that the advert claimed. This
     function does NOT check the range of the timestamp. The callers decide,
     because the peer table still wants an advert whose clock is wrong, while
     the clock estimator must reject that same advert. Both the radio path and
     the bridge path use this function, so the two paths cannot become
     different on framing. */
  bool parseAdvert(const uint8_t* frame, int len,
                   const uint8_t*& pub, uint8_t& hops, uint32_t& their_ts) const;
  /* The same parse, without the self filter. observeTx needs it, because the
     one advert that observeTx cares about is OUR OWN. */
  bool parseAdvertFrame(const uint8_t* frame, int len,
                        const uint8_t*& pub, uint8_t& hops, uint32_t& their_ts) const;
  int  selfIndex(const uint8_t* hash, uint8_t width) const;   // -1 if it is not ours
  bool isSelf(const uint8_t* hash, uint8_t width) const { return selfIndex(hash, width) >= 0; }

  PeerEntry _peers[MAX_PEERS];

  ClockReading _clock_samples[CLOCK_SAMPLES];
  uint8_t     _num_clock_samples = 0;
  void noteClockSample(const uint8_t* pub, uint8_t hops, uint32_t their_ts);

  /* One advert in flight. The code can then time the later copies of that
     advert against the first copy. The key is the originator plus the advert
     timestamp. Together those two identify an advert, and they do not depend
     on the path that the copy arrived by. */
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
  uint32_t _sent_high_s = 0;   // see sentHighWater()

  // The recent flood transmits that wait for a confirmation. A relayed copy can
  // need some time to return. Therefore this is a time window, not one slot.
  static const int TX_RING = 16;
  uint32_t _confirm_window_ms = CONFIRM_WINDOW_DEFAULT_MS;
  struct TxRecord { uint32_t t_ms; int8_t stream; bool confirmed; };
  TxRecord _tx_ring[TX_RING];
  uint8_t  _tx_ring_head = 0, _tx_ring_count = 0;
  uint32_t _flood_sent[MAX_STREAMS] = {0};
  uint32_t _flood_confirmed[MAX_STREAMS] = {0};
  uint32_t _confirm_width[4] = {0};
  // Credit the most recent unconfirmed flood from `stream`, if one is in the
  // window.
  void creditRelay(int stream, uint8_t hash_width);

  uint32_t _hops[HOP_BUCKETS] = {0};
  uint32_t _types[16] = {0};
  uint32_t _frames = 0;
};
