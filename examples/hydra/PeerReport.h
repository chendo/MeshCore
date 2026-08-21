#pragma once

// This file draws the peer table of MeshObserver for the `peers` command.
//
// It is separate from the CLI for two reasons. You can test the format on a
// host. And an error here would destroy the only two facts that the table
// exists to show, and would give no warning. MeshObserver.h states the two
// facts. This file keeps them apart on the way out:
//
//   WE HEAR THEM    This comes only from `direct_rx`. The node was the LAST
//                   entry in a path, so it sent the frame that we received. The
//                   mean SNR describes THAT link and nothing else. A peer that
//                   we see only in the middle of a path (`relays`) has told us
//                   nothing about its link to us. Never draw it as if it had.
//   THEY HEAR US    This comes only from `heard_us`. It is a hash of 2 bytes or
//                   more that comes directly after one of ours. `heard_us_1b`
//                   is the same kind of evidence at 1 byte. A 1-byte hash
//                   collides 1 time in 256. So the table prints it as an
//                   unconfirmed count, and never as a yes.
//
// An entry that is 1 byte wide is also not an identified node. The table shows
// it, because to hide it would hide real traffic. But the table marks it.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <helpers/MeshObserver.h>

// "12s", "5m", "3h", "2d". This is enough detail to tell a live peer from a
// stale one.
inline void peerFmtAge(char* out, size_t cap, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 100)          snprintf(out, cap, "%us", (unsigned)s);
  else if (s < 6000)    snprintf(out, cap, "%um", (unsigned)(s / 60));
  else if (s < 172800)  snprintf(out, cap, "%uh", (unsigned)(s / 3600));
  else                  snprintf(out, cap, "%ud", (unsigned)(s / 86400));
}

// The SNR comes in quarter-dB. Integer maths prints it to one decimal place.
// The %f format on newlib-nano needs a linker flag that this firmware does not
// set.
inline void peerFmtSnrQ4(char* out, size_t cap, int32_t q4) {
  int32_t tenths = (q4 * 10) / 4;
  const char* sign = tenths < 0 ? "-" : "+";
  if (tenths < 0) tenths = -tenths;
  snprintf(out, cap, "%s%d.%d", sign, (int)(tenths / 10), (int)(tenths % 10));
}

inline const char* peerColumnHeader() {
  return "  hash    hop  we-hear-them          they-hear-us     seen   identity";
}

// One row. `now_ms` is millis(). The subtraction is safe at the uint32 wrap.
inline int formatPeerRow(char* out, size_t cap, const MeshObserver::PeerEntry& e,
                         uint32_t now_ms) {
  char hash[8] = {0};
  for (int i = 0; i < e.width && i < 3; i++) {
    snprintf(hash + i * 2, sizeof(hash) - i * 2, "%02x", e.hash[i]);
  }

  char hop[4];
  if (e.min_hops == 0) snprintf(hop, sizeof(hop), "?");
  else snprintf(hop, sizeof(hop), "%u", (unsigned)e.min_hops);

  // -- we hear them: only direct sightings. `relays` is mesh traffic. The table
  // reports it as mesh traffic. The table never adds it to an rx count.
  char us_them[32];
  if (e.direct_rx == 0) {
    snprintf(us_them, sizeof(us_them), "no (relayed x%u)", (unsigned)e.relays);
  } else if (e.snr_n == 0) {
    snprintf(us_them, sizeof(us_them), "rx=%u snr=?", (unsigned)e.direct_rx);
  } else {
    char snr[10];
    peerFmtSnrQ4(snr, sizeof(snr), e.snr4_sum / (int32_t)e.snr_n);
    snprintf(us_them, sizeof(us_them), "rx=%u snr=%s", (unsigned)e.direct_rx, snr);
  }

  // -- they hear us: only a match of 2 bytes or more is proof. The table shows
  // the 1-byte count beside it. The count is thus visible, but it gets no
  // credit.
  char them_us[24];
  if (e.heard_us > 0) {
    if (e.heard_us_1b > 0) {
      snprintf(them_us, sizeof(them_us), "yes x%u +%u?", (unsigned)e.heard_us,
               (unsigned)e.heard_us_1b);
    } else {
      snprintf(them_us, sizeof(them_us), "yes x%u", (unsigned)e.heard_us);
    }
  } else if (e.heard_us_1b > 0) {
    snprintf(them_us, sizeof(them_us), "unproven x%u", (unsigned)e.heard_us_1b);
  } else {
    snprintf(them_us, sizeof(them_us), "no");
  }

  char seen[8];
  peerFmtAge(seen, sizeof(seen), now_ms - e.last_ms);

  // -- the identity, which comes from adverts. It is absent until the node
  // adverts. A 1-byte table entry does not identify a node, even when it
  // carries a name.
  char ident[48];
  bool have_pub = false;
  for (int i = 0; i < 6; i++) if (e.pub[i]) { have_pub = true; break; }
  if (e.name[0]) {
    snprintf(ident, sizeof(ident), "%.*s", (int)sizeof(e.name), e.name);
  } else if (have_pub) {
    snprintf(ident, sizeof(ident), "%02x%02x%02x%02x", e.pub[0], e.pub[1], e.pub[2], e.pub[3]);
  } else {
    snprintf(ident, sizeof(ident), "?");
  }

  return snprintf(out, cap, "  %-6s %-4s %-21s %-16s %-6s %s%s",
                  hash, hop, us_them, them_us, seen, ident,
                  e.width < 2 ? "  [1-byte hash: unproven]" : "");
}

// The summary line. It reports the hash widths, because a table with mostly
// 1-byte entries is a much weaker picture than the same count at 3 bytes.
inline int formatPeerSummary(char* out, size_t cap, const MeshObserver& obs) {
  return snprintf(out, cap,
                  "peers: %d of %d  hear-us %d  widths 1B=%d 2B=%d 3B=%d  "
                  "evicted %u refused %u  frames %u",
                  obs.numPeers(), MeshObserver::MAX_PEERS,
                  obs.confirmedPeerCount(),
                  obs.widthCount(1), obs.widthCount(2), obs.widthCount(3),
                  (unsigned)obs.evictions(), (unsigned)obs.refusedInserts(),
                  (unsigned)obs.framesObserved());
}
