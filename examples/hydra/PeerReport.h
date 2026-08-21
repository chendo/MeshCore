#pragma once

// Rendering MeshObserver's peer table for the `peers` command.
//
// Split out from the CLI so the formatting can be tested on a host, and
// because getting it wrong would quietly destroy the only two facts the table
// is for. MeshObserver.h states them; this file exists to keep them apart on
// the way out:
//
//   WE HEAR THEM    only from `direct_rx` — the node was the LAST entry in a
//                   path, so it transmitted the frame we received. The mean SNR
//                   describes THAT link and nothing else. A peer seen only
//                   mid-path (`relays`) has told us nothing about its link to
//                   us and must never be rendered as if it had.
//   THEY HEAR US    only from `heard_us` — a >=2-byte hash appearing right
//                   after one of ours. `heard_us_1b` is the same shape of
//                   evidence at 1 byte, which collides 1 in 256, so it is
//                   printed as an unconfirmed count and never as a yes.
//
// A 1-byte-wide entry is likewise not an identified node. It is shown, because
// refusing to show it would hide real traffic, but it is marked.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <helpers/MeshObserver.h>

// "12s", "5m", "3h", "2d" — enough resolution to tell live from stale.
inline void peerFmtAge(char* out, size_t cap, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 100)          snprintf(out, cap, "%us", (unsigned)s);
  else if (s < 6000)    snprintf(out, cap, "%um", (unsigned)(s / 60));
  else if (s < 172800)  snprintf(out, cap, "%uh", (unsigned)(s / 3600));
  else                  snprintf(out, cap, "%ud", (unsigned)(s / 86400));
}

// SNR arrives as quarter-dB and is printed to one decimal by integer maths:
// %f on newlib-nano needs a linker flag this firmware does not set.
inline void peerFmtSnrQ4(char* out, size_t cap, int32_t q4) {
  int32_t tenths = (q4 * 10) / 4;
  const char* sign = tenths < 0 ? "-" : "+";
  if (tenths < 0) tenths = -tenths;
  snprintf(out, cap, "%s%d.%d", sign, (int)(tenths / 10), (int)(tenths % 10));
}

inline const char* peerColumnHeader() {
  return "  hash    hop  we-hear-them          they-hear-us     seen   identity";
}

// One row. `now_ms` is millis(); subtraction is wrap-safe on uint32.
inline int formatPeerRow(char* out, size_t cap, const MeshObserver::PeerEntry& e,
                         uint32_t now_ms) {
  char hash[8] = {0};
  for (int i = 0; i < e.width && i < 3; i++) {
    snprintf(hash + i * 2, sizeof(hash) - i * 2, "%02x", e.hash[i]);
  }

  char hop[4];
  if (e.min_hops == 0) snprintf(hop, sizeof(hop), "?");
  else snprintf(hop, sizeof(hop), "%u", (unsigned)e.min_hops);

  // -- we hear them: direct sightings only. `relays` is mesh activity and is
  // reported as such, never folded into an rx count.
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

  // -- they hear us: only >=2-byte matches are proof. The 1-byte tally is
  // carried alongside so it is visible without being credited.
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

  // -- identity, harvested from adverts. Absent until the node adverts; a
  // 1-byte table entry is not an identification even when it carries a name.
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

// The summary line. Hash widths are reported because a table dominated by
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
