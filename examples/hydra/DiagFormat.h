#pragma once

// This file holds everything in the diagnostic bot that has no radio in it: the
// parse of a command, the two rate-limit windows for each verb, and the text of
// every reply. DiagBot.h holds the part that needs a mesh.
//
// The split exists so that a host test can read every reply that this node will
// ever send. No part of this file includes an Arduino or a target header.
//
// TWO HONESTY RULES SHAPE THE PING REPLY.
//
//   THE ROUTE TYPE COMES FIRST. A flood packet collects the hash of each relay
//   as it travels, so the path is in the packet when it arrives. A direct
//   packet loses its path: each relay calls removeSelfFromPath(), and the count
//   is 0 by the time the packet reaches the destination. So `ping` tells you
//   the path on first contact and tells you nothing after the client learns a
//   route. A reply that does not name the route type is misleading, because the
//   reader cannot tell "no relays" from "the relays removed themselves".
//
//   THE CODING RATE IS OURS OR THEIRS. Packet::_cr comes from the LoRa header
//   of the sender. It is 0 when the radio cannot report one. The airtime then
//   comes from OUR configured rate, which is a different measurement. The reply
//   says which of the two it is. It never presents one as the other.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <RateLimiter.h>
#include "PeerReport.h"

// ------------------------------------------------------------- the commands

enum DiagCmd : uint8_t {
  DIAG_PING = 0,
  DIAG_TRACE,
  DIAG_PEERS,
  DIAG_UNKNOWN,   // anything else, an empty message included
};

// The whole message must be one verb. A client sends a plain text message, so
// space at each end is common and the case is not reliable. Nothing else is
// accepted: a bot that answers a prefix answers ordinary chat as well.
inline DiagCmd diagParseCommand(const char* text) {
  if (text == nullptr) return DIAG_UNKNOWN;
  while (*text == ' ' || *text == '\t') text++;
  size_t n = 0;
  while (text[n] && text[n] != ' ' && text[n] != '\t' && text[n] != '\r' && text[n] != '\n') n++;
  const char* tail = text + n;
  while (*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n') tail++;
  if (*tail != 0) return DIAG_UNKNOWN;   // a verb plus an argument is not a verb

  static const struct { const char* word; DiagCmd cmd; } kVerbs[] = {
    { "ping", DIAG_PING }, { "trace", DIAG_TRACE }, { "peers", DIAG_PEERS },
  };
  for (size_t i = 0; i < sizeof(kVerbs) / sizeof(kVerbs[0]); i++) {
    size_t len = strlen(kVerbs[i].word);
    if (len != n) continue;
    size_t k = 0;
    for (; k < n; k++) {
      char c = text[k];
      if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
      if (c != kVerbs[i].word[k]) break;
    }
    if (k == n) return kVerbs[i].cmd;
  }
  return DIAG_UNKNOWN;
}

inline const char* diagUsageText() {
  return "commands: ping, trace, peers. peers needs an admin login on this slot.";
}

// --------------------------------------------------------- the rate limits
//
// Decision 11. The limits are global and not per sender. A per-sender table is
// a table that an attacker fills. Each verb gets two windows: a short one for a
// burst, and a day for the budget.
//
// A DENIED REQUEST GETS NO REPLY. The reply is the cost that the limit exists
// to control, so an "I refuse" message spends exactly what the refusal saves.
// Silence is the refusal.
//
// The clock supplies the time in seconds. A node whose clock nobody set reads
// the same second for ever, so the window never expires and the bot goes quiet
// after the first burst. That is the safe direction, and it is what the anon
// and discover limiters of the repeater already do.
//
// The windows are fixed and are not slides. So a burst across a boundary can
// briefly double the nominal rate. Decision 11 accepts that at these numbers.

struct DiagLimits {
  // A ping reply is about 70 B, which is about 550 ms at SF8 and 62.5 kHz.
  RateLimiter ping_burst{1, 30};
  RateLimiter ping_day{30, 86400};
  // A trace spends the airtime of OTHER nodes, twice for each relay. That is
  // why it is far tighter than ping.
  RateLimiter trace_burst{1, 300};
  RateLimiter trace_day{12, 86400};
  // The usage reply and the peers reply share one budget. This node alone pays
  // for both, and an unknown message reaches the usage reply. Without a limit
  // that reply is the amplifier that the direct-only rule closes.
  RateLimiter other_burst{1, 60};
  RateLimiter other_day{20, 86400};

  // The burst window comes first. A request that the burst window refuses does
  // not touch the day budget, so heavy traffic in one minute cannot spend the
  // whole day.
  static bool pass(RateLimiter& burst, RateLimiter& day, uint32_t now_secs) {
    if (!burst.allow(now_secs)) return false;
    return day.allow(now_secs);
  }
  bool allowPing(uint32_t now_secs)  { return pass(ping_burst, ping_day, now_secs); }
  bool allowTrace(uint32_t now_secs) { return pass(trace_burst, trace_day, now_secs); }
  bool allowOther(uint32_t now_secs) { return pass(other_burst, other_day, now_secs); }
};

// ------------------------------------------------------------- the ping reply

struct DiagPingFacts {
  bool     route_flood;    // false = direct, and then the path is gone
  uint8_t  hops;           // the number of relays in the path of a flood packet
  uint8_t  hash_size;      // the width of one path entry, 1..3 bytes
  const uint8_t* path;     // hops * hash_size bytes
  int8_t   snr4;           // SNR * 4
  int16_t  rssi;           // dBm
  uint8_t  cr;             // the 4/x denominator, 5..8. 0 = we know of none.
  bool     cr_observed;    // true = read from the header of the sender
  uint32_t airtime_ms;
  uint8_t  hash[4];        // the first 4 bytes of the packet hash
};

// The reply carries at most this many path entries. A path of 63 entries fills
// the message and pushes out the link data, which a long path makes more
// interesting and not less.
#define DIAG_PING_MAX_PATH  8

// A text message holds MAX_TEXT_LEN characters, and composeMsgPacket() drops a
// longer one. So a reply that overflows is a reply that nobody reads. Every
// list below therefore stops while it still has room, and marks that it
// stopped. This returns the space that is left, and never a negative number.
inline size_t diagRoomLeft(size_t cap, int n) {
  if (n < 0 || (size_t)n >= cap) return 0;
  return cap - (size_t)n;
}

inline int diagFormatPing(char* out, size_t cap, const DiagPingFacts& f) {
  char snr[10];
  peerFmtSnrQ4(snr, sizeof(snr), f.snr4);

  char cr[40];
  if (f.cr_observed) {
    snprintf(cr, sizeof(cr), "cr 4/%u sender", (unsigned)f.cr);
  } else if (f.cr != 0) {
    // The radio reported no rate. This number is what THIS node transmits at,
    // and the airtime below carries that price. It is not a measurement of the
    // sender.
    snprintf(cr, sizeof(cr), "cr 4/%u configured (ours)", (unsigned)f.cr);
  } else {
    snprintf(cr, sizeof(cr), "cr unknown, priced at ours");
  }

  int n;
  if (!f.route_flood) {
    // See the note at the top of this file. A direct packet arrives with an
    // empty path, so "0 hops" is a lie and no hop count is available.
    n = snprintf(out, cap, "ping: direct route, the relays took the path out");
  } else {
    n = snprintf(out, cap, "ping: flood route, %u hops", (unsigned)f.hops);
  }
  if (diagRoomLeft(cap, n) == 0) return n;
  n += snprintf(out + n, cap - n, " | snr %s rssi %ddBm | %s air %ums | hash %02x%02x%02x%02x",
                snr, (int)f.rssi, cr, (unsigned)f.airtime_ms,
                f.hash[0], f.hash[1], f.hash[2], f.hash[3]);

  // The path comes last, because it is the only part whose length nobody
  // controls. A cut then costs path entries and never the link figures.
  if (!f.route_flood || f.hops == 0 || diagRoomLeft(cap, n) == 0) return n;
  n += snprintf(out + n, cap - n, " | via ");
  uint8_t show = f.hops > DIAG_PING_MAX_PATH ? DIAG_PING_MAX_PATH : f.hops;
  uint8_t i = 0;
  for (; i < show; i++) {
    size_t need = (size_t)f.hash_size * 2 + (i ? 1 : 0);
    if (diagRoomLeft(cap, n) < need + 4) break;    // 4 = ",.." and the terminator
    if (i) n += snprintf(out + n, cap - n, ",");
    for (uint8_t b = 0; b < f.hash_size; b++) {
      n += snprintf(out + n, cap - n, "%02x", f.path[i * f.hash_size + b]);
    }
  }
  if (i < f.hops && diagRoomLeft(cap, n) > 3) n += snprintf(out + n, cap - n, ",..");
  return n;
}

// ------------------------------------------------------------ the trace reply
//
// A trace goes out along the path to the client and comes back to this node.
// Each node on the way appends the SNR at which it heard the previous
// transmission. So entry i is the reading that hop i took, and the last figure
// is the reading that THIS node took of the last relay.

#define DIAG_TRACE_MAX_HOPS  12

inline int diagFormatTrace(char* out, size_t cap, const uint8_t* hashes, uint8_t entry_size,
                           const int8_t* snr4, uint8_t n_hops, int8_t final_snr4,
                           uint8_t relays) {
  // The tail names the round trip, so it must survive a cut. Hold room for it.
  char tail[64];
  {
    char s[10];
    peerFmtSnrQ4(s, sizeof(s), final_snr4);
    snprintf(tail, sizeof(tail), " us %s (snr at each hop, out and back over %u relay%s)",
             s, (unsigned)relays, relays == 1 ? "" : "s");
  }
  size_t reserve = strlen(tail) + 5;   // 5 = " ..," and the terminator

  int n = snprintf(out, cap, "trace:");
  uint8_t show = n_hops > DIAG_TRACE_MAX_HOPS ? DIAG_TRACE_MAX_HOPS : n_hops;
  uint8_t i = 0;
  for (; i < show; i++) {
    char s[10];
    peerFmtSnrQ4(s, sizeof(s), snr4[i]);
    size_t need = 1 + (size_t)entry_size * 2 + 1 + strlen(s) + 1;
    if (diagRoomLeft(cap, n) < need + reserve) break;
    n += snprintf(out + n, cap - n, " ");
    for (uint8_t b = 0; b < entry_size; b++) {
      n += snprintf(out + n, cap - n, "%02x", hashes[i * entry_size + b]);
    }
    n += snprintf(out + n, cap - n, " %s,", s);
  }
  if (i < n_hops && diagRoomLeft(cap, n) > 4) n += snprintf(out + n, cap - n, " ..,");
  if (diagRoomLeft(cap, n) > 0) n += snprintf(out + n, cap - n, "%s", tail);
  return n;
}

// Every way that a trace request stops before it goes on air. The requester
// gets one of these, because each one describes a state that the requester can
// act on.
enum DiagTraceRefusal : uint8_t {
  DIAG_TRACE_OK = 0,
  DIAG_TRACE_NO_PATH,      // we have never learned a route to this contact
  DIAG_TRACE_ZERO_HOP,     // the contact is next door: there is nothing between us
  DIAG_TRACE_TOO_LONG,     // the round trip does not fit in one packet
  DIAG_TRACE_IN_FLIGHT,    // one is already out there
  DIAG_TRACE_NO_PACKET,    // the packet pool is empty
  DIAG_TRACE_BAD_WIDTH,    // the route uses a path width that a trace cannot carry
};

// A path entry is 1, 2 or 3 bytes wide in a Packet, where the width is the top
// 2 bits of path_len plus one. A TRACE carries the width in the lower 2 bits of
// its flags, and there the width is 1 << that value. So the two encodings agree
// on 1 byte, on 2 bytes and on 4 bytes, and they disagree on 3. A route of
// 3-byte entries therefore has no trace flags value at all.
//
// Returns 0xFF when no flags value exists.
inline uint8_t diagTraceFlagsForWidth(uint8_t hash_size) {
  switch (hash_size) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
  }
  return 0xFF;
}

inline const char* diagTraceRefusalText(DiagTraceRefusal r) {
  switch (r) {
    case DIAG_TRACE_OK:        return "";
    case DIAG_TRACE_NO_PATH:   return "trace: I have no route to you yet. Send ping first.";
    case DIAG_TRACE_ZERO_HOP:  return "trace: you are next door, so there are no hops to trace.";
    case DIAG_TRACE_TOO_LONG:  return "trace: the route is too long for one out-and-back trace.";
    case DIAG_TRACE_IN_FLIGHT: return "trace: one is already out. Wait for it.";
    case DIAG_TRACE_NO_PACKET: return "trace: no free packet right now. Try again.";
    case DIAG_TRACE_BAD_WIDTH: return "trace: your route uses a 3-byte path, which a trace cannot carry.";
  }
  return "trace: refused.";
}

// The out-and-back path for a server-initiated trace.
//
// The route to the client is S -> R1 -> .. -> Rn -> C. The trace turns round at
// Rn and comes home, so the entries are R1..Rn then Rn-1..R1. The node that
// receives the packet after the last entry is S, which is us, and onTraceRecv()
// fires there. The client is never in the list, so the client needs no trace
// support at all.
//
// Two facts make this work. Mesh::calculatePacketHash() mixes path_len into the
// hash of a TRACE packet, so a relay that sees the packet a second time on the
// way back does not treat it as a duplicate. And each relay appends one SNR
// byte to Packet::path, so the number of entries must stay below MAX_PATH_SIZE.
//
// Returns the byte length written, or 0 when the round trip does not fit.
inline uint8_t diagBuildTracePath(uint8_t* out, size_t cap, const uint8_t* out_path,
                                  uint8_t relays, uint8_t entry_size) {
  if (relays == 0) return 0;
  uint16_t entries = (uint16_t)relays * 2 - 1;
  if (entries >= MAX_PATH_SIZE) return 0;                 // the SNR array must hold one for each
  uint16_t bytes = entries * entry_size;
  if (bytes > cap) return 0;
  if (9 + bytes > MAX_PACKET_PAYLOAD) return 0;           // 9 B of tag, auth code and flags

  uint16_t w = 0;
  for (uint8_t i = 0; i < relays; i++) {                  // out: R1..Rn
    memcpy(&out[w], &out_path[i * entry_size], entry_size);
    w += entry_size;
  }
  for (int i = (int)relays - 2; i >= 0; i--) {            // back: Rn-1..R1
    memcpy(&out[w], &out_path[i * entry_size], entry_size);
    w += entry_size;
  }
  return (uint8_t)w;
}

// ------------------------------------------------------------ the peers reply
//
// Decision E. This describes third parties who never agreed to be described, so
// it needs a login. The format keeps apart the two facts that MeshObserver
// keeps apart. Read the note at the top of PeerReport.h before you change this.
// A row that joins them destroys the only thing that the table is for.

inline const char* diagPeersDeniedText() {
  return "peers: this reports on other nodes, so it needs an admin login on this slot.";
}

// The gate. The argument is the permissions byte of the sender in the ACL of
// this slot, or null when the sender is not in it and when the slot carries no
// ACL at all. A chat slot has no ACL, so on a chat slot every caller gets null
// and the answer is always no.
//
// Only an admin passes. A read-only or a read-write member of a room is a
// member of THAT room. It is not a person who may read a map of every node that
// this antenna can hear.
#define DIAG_PEERS_PERM_MASK  3
#define DIAG_PEERS_PERM_ADMIN 3

inline bool diagPeersAllowed(const uint8_t* sender_permissions) {
  if (sender_permissions == nullptr) return false;
  return (*sender_permissions & DIAG_PEERS_PERM_MASK) == DIAG_PEERS_PERM_ADMIN;
}

inline int diagFormatPeerLine(char* out, size_t cap, const MeshObserver::PeerEntry& e,
                              uint32_t now_ms) {
  char hash[8] = {0};
  for (int i = 0; i < e.width && i < 3; i++) {
    snprintf(hash + i * 2, sizeof(hash) - i * 2, "%02x", e.hash[i]);
  }

  char hop[4];
  if (e.min_hops == 0) snprintf(hop, sizeof(hop), "h?");
  else snprintf(hop, sizeof(hop), "h%u", (unsigned)e.min_hops);

  // WE HEAR THEM. Only a direct sighting counts. A node in the middle of the
  // path of somebody else says nothing about its link to us, so it gets no rx
  // count and no SNR.
  char ours[24];
  if (e.direct_rx == 0) {
    snprintf(ours, sizeof(ours), "relay%u", (unsigned)e.relays);
  } else if (e.snr_n == 0) {
    snprintf(ours, sizeof(ours), "rx%u snr?", (unsigned)e.direct_rx);
  } else {
    char snr[10];
    peerFmtSnrQ4(snr, sizeof(snr), e.snr4_sum / (int32_t)e.snr_n);
    snprintf(ours, sizeof(ours), "rx%u snr%s", (unsigned)e.direct_rx, snr);
  }

  // THEY HEAR US. A hash of 2 bytes or more that follows one of ours is proof.
  // A 1-byte match is a 1-in-256 coincidence, so it stays a separate count with
  // a question mark. It never becomes a yes.
  char theirs[24];
  if (e.heard_us > 0 && e.heard_us_1b > 0) {
    snprintf(theirs, sizeof(theirs), "yes%u+%u?", (unsigned)e.heard_us, (unsigned)e.heard_us_1b);
  } else if (e.heard_us > 0) {
    snprintf(theirs, sizeof(theirs), "yes%u", (unsigned)e.heard_us);
  } else if (e.heard_us_1b > 0) {
    snprintf(theirs, sizeof(theirs), "1b%u?", (unsigned)e.heard_us_1b);
  } else {
    snprintf(theirs, sizeof(theirs), "no");
  }

  char age[8];
  peerFmtAge(age, sizeof(age), now_ms - e.last_ms);

  return snprintf(out, cap, "%s %s %s hear-us:%s %s%s", hash, hop, ours, theirs, age,
                  e.width < 2 ? " !1B" : "");
}
