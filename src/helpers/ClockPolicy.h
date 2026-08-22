#pragma once

// Clock convergence: how a node works out the true time from what its peers
// say, and how much of that answer it is allowed to apply right now.
//
// This file is pure arithmetic. It has no hardware, no clock, no millis(), no
// I/O and no state. The caller collects the samples, keeps the counters and
// writes the RTC. This file only decides. That split is what makes the rules
// below testable on a host, and this feature must not reach a repeater until
// the rules are tested.
//
// THE EVIDENCE. An ADVERT carries a timestamp that its originator signed.
// Therefore every advert is one reading of one remote clock against ours. That
// is the only time reference that the mesh gives a node for free.
//
// WHAT THE FIELD LOOKS LIKE. The thresholds here come from a survey of 407
// nodes on a true regional mesh:
//   - the median offset was -12s and the median absolute deviation was 7s;
//   - the mean was -46s and the range was -3533..+1239, so no estimate that
//     uses a mean can work;
//   - 21% of the nodes were outliers by 3*MAD;
//   - the extreme values were not single bad clocks. They were GROUPS that
//     shared one offset (-227s x6, -211s x5, +335s x4). These are sub-networks
//     that agreed with each other and were wrong together;
//   - 10 nodes reported rates that no crystal can produce. The largest was
//     58485 s/day.
// The group failure is the important one. Median and MAD assume one population
// with outliers scattered around it, and they fail quietly when the population
// truly splits: the MAD grows until the clip threshold accepts everybody, and
// the median lands between the groups, on a value that not one node holds.
// Therefore this file selects the densest CLUSTER first and does the statistics
// inside it. See clockEstimate().

#include <stdint.h>
#include <stddef.h>

namespace mesh {

/* A clock that reads before this moment is not wrong. It is NO time. These
   boards have no hardware RTC, so every reboot returns them to the built-in
   1715770351 of VolatileRTCClock, which is 15 May 2024, and they hold that date
   until something else sets them. 1 Jan 2026 is above every such default and
   below any live deployment, so it separates "never set" from "set and
   drifting" without knowledge of the build date. */
static const uint32_t CLOCK_SET_EPOCH = 1767225600UL;   // 2026-01-01

/* The largest correction that convergence may make in one hour, once the clock
   is set. The limit exists because a clock that moves fast is a clock that
   other nodes cannot follow, and because a wrong consensus must never be able
   to drag a good clock a long way before a person notices. */
static const int32_t  CLOCK_SLEW_MAX_S_PER_HOUR = 60;
static const uint32_t CLOCK_SLEW_WINDOW_MS = 3600000UL;   // one hour

/* A person who sets the clock beats the neighbourhood for this long. The
   operator has a true time source and the mesh does not, so an admin action
   must not decay into a majority vote a few minutes later. */
static const uint32_t CLOCK_ADMIN_HOLD_MS = 7UL * 24UL * 3600UL * 1000UL;   // 7 days

/* Agreement to this many seconds is agreement. Both clocks read to the second,
   and the mesh in the survey runs at a MAD of 7s, so a correction of one or two
   seconds moves nothing that anybody can measure. */
static const int32_t  CLOCK_DEADBAND_S = 2;

/* Two readings this close agree. The value is the MAD of the survey mesh, 7s,
   rounded up. The cluster search below uses it as a half-width, so the readings
   that win are inside a band of 16s. That bound matters twice: it is the width
   that the older-end bias can cost (see clockEstimate), and it removes the need
   for a separate spread threshold, because a cluster cannot be wider than the
   band that selected it. */
static const int32_t  CLOCK_CLUSTER_BAND_S = 8;

/* The fraction of the evidence, by weight, that the winning cluster must hold.
   This is the "sources that disagree do not move the clock" rule. A mesh that
   splits into two beliefs gives neither side a majority, and the correct action
   on a split is none. */
static const uint8_t  CLOCK_MIN_AGREE_PCT = 60;

/* The number of separate peers that must sit inside the winning cluster. One
   peer is not evidence: it is one other node with one other opinion, and it can
   be as wrong as we are. The survey put the false-fire rate of a step at 0.2%
   of rounds with eight sources and 4.3% with four, so three is a floor and not
   a comfort. */
static const uint8_t  CLOCK_MIN_SOURCES = 3;

/* An unset clock may act on two. There is nothing to protect: until the node
   leaves 15 May 2024 its adverts carry timestamps that the rest of the mesh
   rejects as replays, so the node is not merely wrong, it is invisible. Every
   round that the node waits for a third source is a round off the air. */
static const uint8_t  CLOCK_UNSET_MIN_SOURCES = 2;

/* Above this hop count two error terms are larger than the reading is worth.
   They are the accumulated propagation correction, and the uncertainty in the
   constant that makes that correction. */
static const uint8_t  CLOCK_MAX_HOPS = 8;

/* The vote weight of an advert heard straight off the radio of its originator.
   A relayed advert is worth CLOCK_WEIGHT_DIRECT / (1 + hops). Each hop adds one
   full random relay wait to the propagation correction, so the variance of that
   correction grows in proportion to the hop count, and the weight that gives
   the smallest error is one over the variance. The value 12 is chosen so that
   the division stays exact for the first four hops: 12, 6, 4, 3. */
static const uint16_t CLOCK_WEIGHT_DIRECT = 12;

/* The default per-hop propagation delay, in milliseconds. A repeater waits
   rng(0, 5*airtime*tx_delay_factor) before it relays, and then spends the
   airtime on the transmission. At the repeater default factor of 0.5 the mean
   is 2.25*airtime, which is about 1.5s for a 130-byte advert at SF7/BW62.5.
   MeshObserver measures the true value and passes it in. */
static const uint16_t CLOCK_HOP_DELAY_DEFAULT_MS = 1500;

// The largest number of samples that one estimate reads. MeshObserver holds 24.
static const int CLOCK_POLICY_MAX_SAMPLES = 32;

/**
 * \brief  One reading of one peer's clock.
 *
 * `offset_s` is what that peer said MINUS what our clock says, at the moment of
 * use. The caller must subtract at the point of use and never store a
 * difference, because a difference has a meaning only against the clock that
 * measured it: a person, or convergence itself, steps our clock and every
 * stored difference silently becomes a lie.
 *
 * `weight` is the credibility of the source before the hop penalty. Pass 1 for
 * an ordinary peer. A caller that collapses a group of peers onto one reading
 * passes the size of that group, and a caller that trusts one transport more
 * than another says so here.
 */
struct ClockSample {
  int32_t offset_s;
  uint8_t hops;      // 0 = we heard the transmission of the originator itself
  uint8_t weight;
};

/**
 * \brief  The seconds to add to a relayed reading to undo its journey.
 *
 * The originator stamped the advert before the advert started. Therefore the
 * reading arrives late by the length of the journey, and it makes every distant
 * node look slow. This puts the time back.
 */
inline int32_t clockHopCorrectionS(uint8_t hops, uint16_t hop_delay_ms) {
  return (int32_t)(((uint32_t)hops * (uint32_t)hop_delay_ms + 500) / 1000);
}

/**
 * \brief  The vote weight of a sample, after the penalty for its hops.
 *
 * Returns 0 for a sample that is too far away to use, and for a source that the
 * caller gave no weight.
 */
inline uint16_t clockSampleWeight(uint8_t hops, uint8_t source_weight) {
  if (hops > CLOCK_MAX_HOPS || source_weight == 0) return 0;
  uint32_t w = ((uint32_t)CLOCK_WEIGHT_DIRECT * source_weight) / (uint32_t)(1 + hops);
  if (w == 0) w = 1;              // a distant peer still holds one vote
  if (w > 0xFFFFu) w = 0xFFFFu;
  return (uint16_t)w;
}

/**
 * \brief  What the neighbourhood says our clock error is.
 */
struct ClockEstimate {
  bool     valid;         // the result passed the quorum and the agreement gate
  int32_t  offset_s;      // the seconds to ADD to our clock to join the majority
  int32_t  spread_s;      // the width of the winning cluster, oldest to newest
  uint32_t weight_seen;   // the total weight of every usable sample
  uint32_t weight_used;   // the weight inside the winning cluster
  uint8_t  n_seen;        // the peers that offered a usable reading
  uint8_t  n_used;        // the peers inside the winning cluster
  uint8_t  n_direct;      // how many of those we heard at zero hops
  uint8_t  agree_pct;     // weight_used * 100 / weight_seen
};

/**
 * \brief  Estimate our clock error from a set of peer readings.
 *
 * The method has three steps.
 *
 * FIRST, each reading is corrected for its journey and given a weight that
 * falls with its hop count. See clockHopCorrectionS and clockSampleWeight.
 *
 * SECOND, the densest cluster wins. The code centres a band of
 * CLOCK_CLUSTER_BAND_S on each reading in turn, adds the weight inside that
 * band, and keeps the best band. This answers the question that a mesh actually
 * asks -- what do most nodes agree the time is -- and it survives the split
 * population that defeats a median. A wrong sub-network can still win by
 * outnumbering the right one, which is what a majority means. It cannot do much
 * with the win, because the caller may move only 60 seconds in an hour.
 *
 * THIRD, the answer is the weighted LOWER QUARTILE of the cluster, not its
 * median. This is deliberate, and it is the one place where the estimate is not
 * the best guess available.
 *
 *   The result is that the node lands a few seconds BEHIND the consensus
 *   instead of on it. Every correction after that is therefore a FORWARD one,
 *   and forward is the safe direction: every peer that a clock has already
 *   talked to rejects it after it moves backwards, because their replay defences
 *   refuse a timestamp that is not newer than the last one they saw.
 *   A node that approaches the true time from below never has to make that
 *   move. A node that aims at the median crosses it and steps back every time
 *   its crystal runs fast.
 *
 *   The cost is bounded by the cluster band. A cluster is at most 16s wide, so
 *   the bias can cost at most 16s of accuracy against a mesh whose own MAD is
 *   7s. The bias also scales with the disagreement, which is what "when
 *   uncertain, bias toward the older end" means: readings that agree exactly
 *   give a quartile equal to the median, and no bias at all.
 *
 * \param  in            the readings, in any order
 * \param  n             how many
 * \param  min_sources   peers that the winning cluster must hold
 * \param  hop_delay_ms  the measured per-hop propagation delay
 */
inline ClockEstimate clockEstimate(const ClockSample* in, int n,
                                   uint8_t min_sources = CLOCK_MIN_SOURCES,
                                   uint16_t hop_delay_ms = CLOCK_HOP_DELAY_DEFAULT_MS) {
  ClockEstimate e;
  e.valid = false; e.offset_s = 0; e.spread_s = 0;
  e.weight_seen = 0; e.weight_used = 0;
  e.n_seen = 0; e.n_used = 0; e.n_direct = 0; e.agree_pct = 0;

  if (in == NULL || n <= 0) return e;
  if (n > CLOCK_POLICY_MAX_SAMPLES) n = CLOCK_POLICY_MAX_SAMPLES;

  int32_t  v[CLOCK_POLICY_MAX_SAMPLES];    // the corrected readings, oldest first
  uint16_t w[CLOCK_POLICY_MAX_SAMPLES];
  uint8_t  z[CLOCK_POLICY_MAX_SAMPLES];    // 1 = the node heard it at zero hops
  int m = 0;

  for (int i = 0; i < n; i++) {
    uint16_t cw = clockSampleWeight(in[i].hops, in[i].weight);
    if (cw == 0) continue;
    int32_t cv = in[i].offset_s + clockHopCorrectionS(in[i].hops, hop_delay_ms);
    // An insertion sort. CLOCK_POLICY_MAX_SAMPLES bounds n, so the cost is
    // small, and the sort keeps the parallel arrays together.
    int k = m++;
    while (k > 0 && v[k - 1] > cv) { v[k] = v[k - 1]; w[k] = w[k - 1]; z[k] = z[k - 1]; k--; }
    v[k] = cv; w[k] = cw; z[k] = (in[i].hops == 0) ? 1 : 0;
    e.weight_seen += cw;
  }
  e.n_seen = (uint8_t)(m > 255 ? 255 : m);
  if (m == 0) return e;

  /* The densest band. The readings are sorted, so every band is a run of
     neighbours and the search needs no second sort. The comparison is strictly
     greater than, so the FIRST centre wins a tie. The first centre is the
     oldest one, which applies the older-end rule where two bands are otherwise
     equal. */
  int best_lo = 0, best_hi = 0;
  uint32_t best_w = 0;
  for (int c = 0; c < m; c++) {
    int lo = c, hi = c;
    while (lo > 0 && (int64_t)v[c] - v[lo - 1] <= CLOCK_CLUSTER_BAND_S) lo--;
    while (hi + 1 < m && (int64_t)v[hi + 1] - v[c] <= CLOCK_CLUSTER_BAND_S) hi++;
    uint32_t ww = 0;
    for (int i = lo; i <= hi; i++) ww += w[i];
    if (ww > best_w) { best_w = ww; best_lo = lo; best_hi = hi; }
  }

  int used = best_hi - best_lo + 1;
  e.n_used     = (uint8_t)(used > 255 ? 255 : used);
  e.weight_used = best_w;
  e.spread_s   = v[best_hi] - v[best_lo];
  for (int i = best_lo; i <= best_hi; i++) if (z[i]) e.n_direct++;
  e.agree_pct  = (uint8_t)((e.weight_seen == 0) ? 0 : (best_w * 100) / e.weight_seen);

  // The weighted lower quartile of the cluster. See the note above on why the
  // answer is not the median.
  uint32_t need = (best_w + 3) / 4;
  uint32_t acc = 0;
  e.offset_s = v[best_lo];
  for (int i = best_lo; i <= best_hi; i++) {
    acc += w[i];
    if (acc >= need) { e.offset_s = v[i]; break; }
  }

  if (used < (int)min_sources) return e;
  if (e.agree_pct < CLOCK_MIN_AGREE_PCT) return e;
  e.valid = true;
  return e;
}

// ---- acting on the estimate -------------------------------------------------

enum ClockAction : uint8_t {
  CLOCK_HOLD = 0,   // change nothing
  CLOCK_STEP,       // move the whole way in one go; only an unset clock does this
  CLOCK_SLEW,       // move part of the way, inside the rate limit
};

// Why the decision was CLOCK_HOLD. The CLI reports this, so an operator can see
// the difference between "no evidence" and "evidence that I refuse to act on".
enum ClockHold : uint8_t {
  CLOCK_HOLD_NONE = 0,
  CLOCK_HOLD_NO_QUORUM,   // too few peers, or the peers do not agree
  CLOCK_HOLD_IN_BAND,     // the error is inside the deadband
  CLOCK_HOLD_ADMIN,       // a person set the clock, and that set still holds
  CLOCK_HOLD_REPLAY,      // the move would go below a timestamp we already sent
  CLOCK_HOLD_RATE,        // the rate limit has nothing left this round
};

struct ClockDecision {
  ClockAction action;
  ClockHold   hold;
  int32_t     target_s;   // the whole correction that the estimate asks for
  int32_t     apply_s;    // the seconds to add to the clock NOW; 0 on a hold
};

/**
 * \brief  Everything about our own node that the decision needs.
 *
 * The two elapsed-time fields are counters that the caller carries forward, NOT
 * millis() readings. See clockAdvanceElapsed for why.
 */
struct ClockContext {
  uint32_t now_s;          // what our own clock reads, in epoch seconds
  uint32_t since_move_ms;  // the time since convergence last moved the clock
  uint32_t admin_hold_ms;  // the time left on the admin hold; 0 = no hold
  /* The newest timestamp that this node has put on the air. 0 means that the
     node has sent nothing, which is the state of a node that has just booted.
     THIS IS THE REPLAY FLOOR, and clockDecide will not take the clock to or
     below it. See that function for what breaks without it. */
  uint32_t sent_high_s;
};

/**
 * \brief  Add one poll interval to a saturating elapsed-time counter.
 *
 * Convergence must survive the millis() wrap at 49.7 days, and a node stays up
 * for longer than that. Arithmetic on absolute millis() readings cannot survive
 * it: a hold that started at millis()==0 and lasts 7 days looks unexpired again
 * the moment the counter wraps back past 0.
 *
 * Therefore the caller never keeps an absolute reading. It keeps a counter, and
 * each poll adds the DIFFERENCE between this millis() and the previous one. An
 * unsigned difference is correct across the wrap, and the poll interval is
 * minutes, so the difference is never near the wrap period.
 */
inline uint32_t clockAdvanceElapsed(uint32_t elapsed_ms, uint32_t delta_ms) {
  uint32_t sum = elapsed_ms + delta_ms;
  if (sum < elapsed_ms) return 0xFFFFFFFFu;    // saturate; do not wrap
  return sum;
}

/** \brief  Take one poll interval off a hold, and stop at zero. */
inline uint32_t clockReduceHold(uint32_t left_ms, uint32_t delta_ms) {
  return (left_ms > delta_ms) ? (left_ms - delta_ms) : 0;
}

/** \brief  The time between two millis() readings. Correct across the wrap. */
inline uint32_t clockPollDelta(uint32_t now_ms, uint32_t prev_ms) {
  return (uint32_t)(now_ms - prev_ms);
}

/**
 * \brief  The seconds of correction that the rate limit has released.
 *
 * The allowance grows at CLOCK_SLEW_MAX_S_PER_HOUR and stops at one hour's
 * worth. The cap is the part that matters: without it a node that sat idle for
 * a day could spend the whole day's allowance in one move, which is the jump
 * that the rate limit exists to prevent.
 */
inline int32_t clockSlewAllowanceS(uint32_t since_move_ms) {
  uint64_t s = ((uint64_t)since_move_ms * (uint64_t)CLOCK_SLEW_MAX_S_PER_HOUR)
             / (uint64_t)CLOCK_SLEW_WINDOW_MS;
  if (s > (uint64_t)CLOCK_SLEW_MAX_S_PER_HOUR) s = (uint64_t)CLOCK_SLEW_MAX_S_PER_HOUR;
  return (int32_t)s;
}

/** \brief  True while the clock holds a value that nobody ever set. */
inline bool clockIsUnset(uint32_t now_s) {
  return now_s < CLOCK_SET_EPOCH;
}

/**
 * \brief  Add a correction to a clock reading.
 *
 * The arithmetic is modular, so it stays correct at the 2106 wrap of a 32-bit
 * epoch. A signed cast would be undefined there.
 */
inline uint32_t clockApply(uint32_t now_s, int32_t adj_s) {
  return (uint32_t)(now_s + (uint32_t)adj_s);
}

/**
 * \brief  Decide what to do about the estimate.
 *
 * THE REPLAY HAZARD, and why ClockContext::sent_high_s exists.
 *
 * MeshCore rejects a packet whose sender_timestamp is not newer than the last
 * one that the receiver stored for that sender. BaseChatMesh does this to
 * adverts, against ContactInfo::last_advert_timestamp, and the servers do it to
 * logins and requests against ClientInfo::last_timestamp. Therefore every peer
 * that already knows a node rejects that node after it moves its clock
 * backwards, and the rejection continues until its clock climbs back past its
 * own previous high-water mark. At 60 seconds an hour, a 10-minute correction would
 * cost 10 hours of partial invisibility.
 *
 * The fix is exact rather than cautious. The caller records the newest
 * timestamp that this node has actually put on the air, and this function will
 * not take the clock to or below it. Any value above it is invisible to every
 * peer, because no peer holds a mark higher than the largest value we ever
 * sent. Anything at or below it is rejected by somebody, so it is refused here
 * and reported as CLOCK_HOLD_REPLAY.
 *
 * Two properties follow, and both matter:
 *   - the room to move backwards grows as the node stays quiet. sent_high_s
 *     does not age, our clock does, so an hour after the last transmission the
 *     node may undo up to an hour;
 *   - the wall clock stays truthful. The alternative fix, which is to stamp
 *     outgoing packets from a monotonic counter instead of the clock, would put
 *     a timestamp on our adverts that is minutes ahead of what we believe the
 *     time to be. Our adverts are what every neighbour samples to set ITS
 *     clock, so that fix would poison the input of the whole feature.
 *
 * An admin set is not bound by any of this. The operator has a true time
 * source, and rule 3 gives the operator the last word.
 */
inline ClockDecision clockDecide(const ClockEstimate& est, const ClockContext& ctx) {
  ClockDecision d;
  d.action = CLOCK_HOLD; d.hold = CLOCK_HOLD_NONE;
  d.target_s = est.valid ? est.offset_s : 0;
  d.apply_s = 0;

  if (!est.valid) { d.hold = CLOCK_HOLD_NO_QUORUM; return d; }

  /* An unset clock is its own state, and none of the caution below applies to
     it. There is no rate limit, because the node must cross years and not
     seconds. There is no replay floor, because every timestamp this node has
     sent was already refused by the whole mesh as a replay of 2024. There is no
     admin hold, because a person who wants 2024 reaches it through clkreboot,
     which reboots and therefore clears the hold. */
  if (clockIsUnset(ctx.now_s)) {
    d.action = CLOCK_STEP;
    d.apply_s = est.offset_s;
    return d;
  }

  if (ctx.admin_hold_ms > 0) { d.hold = CLOCK_HOLD_ADMIN; return d; }

  if (est.offset_s >= -CLOCK_DEADBAND_S && est.offset_s <= CLOCK_DEADBAND_S) {
    d.hold = CLOCK_HOLD_IN_BAND;
    return d;
  }

  int32_t allow = clockSlewAllowanceS(ctx.since_move_ms);
  if (allow <= 0) { d.hold = CLOCK_HOLD_RATE; return d; }

  int32_t adj = est.offset_s;
  if (adj > allow) adj = allow;
  if (adj < -allow) adj = -allow;

  if (adj < 0 && ctx.sent_high_s != 0) {
    /* How far below our present reading the replay floor sits. The subtraction
       is unsigned and the cast is signed, so the result is correct across the
       2106 wrap. It is normally negative, because we stamped our last packet
       before now. */
    int32_t behind = (int32_t)(ctx.sent_high_s - ctx.now_s);
    int32_t floor_adj = behind + 1;          // the clock must finish ABOVE the mark
    if (floor_adj > adj) adj = floor_adj;
    if (adj >= 0) { d.hold = CLOCK_HOLD_REPLAY; return d; }
  }

  d.action = CLOCK_SLEW;
  d.apply_s = adj;
  return d;
}

}
