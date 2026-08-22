// These tests cover the diagnostic chat bot: the parse of a command, the two
// rate-limit windows for each verb, the out-and-back path that a server-started
// trace follows, and the text of every reply.
//
// The reply text is the product. A person reads it on a phone and acts on it.
// Two of the strings carry a claim that the code alone cannot check:
//
//   * the ROUTE TYPE, because a flood packet still holds its path and a direct
//     packet does not. A reply that reports "0 hops" for a direct packet is
//     wrong, and it is wrong in a way that looks correct.
//   * the CODING RATE, because Packet::_cr is the rate of the SENDER, and 0
//     means that the radio reported none. The airtime then carries OUR rate.
//     The reply must not present one as the other.
//
// A test for each. Both fail loudly.

#include <gtest/gtest.h>
#include <DiagFormat.h>
#include <string>

namespace {
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

DiagPingFacts pingFacts() {
  DiagPingFacts f;
  memset(&f, 0, sizeof(f));
  f.route_flood = true;
  f.hops = 0;
  f.hash_size = 1;
  f.path = nullptr;
  f.snr4 = 30;        // +7.5 dB
  f.rssi = -92;
  f.cr = 5;
  f.cr_observed = true;
  f.airtime_ms = 552;
  f.hash[0] = 0x3f; f.hash[1] = 0x9c; f.hash[2] = 0x2a; f.hash[3] = 0x1b;
  return f;
}

std::string ping(const DiagPingFacts& f) {
  char buf[300];
  diagFormatPing(buf, sizeof(buf), f);
  return std::string(buf);
}

MeshObserver::PeerEntry blankPeer() {
  MeshObserver::PeerEntry e;
  memset(&e, 0, sizeof(e));
  e.width = 3;
  e.hash[0] = 0xa1; e.hash[1] = 0xb2; e.hash[2] = 0xc3;
  e.min_hops = 1;
  e.last_ms = 95000;
  return e;
}

std::string peerLine(const MeshObserver::PeerEntry& e, uint32_t now = 100000) {
  char buf[120];
  diagFormatPeerLine(buf, sizeof(buf), e, now);
  return std::string(buf);
}
}

// ------------------------------------------------------------- the parse

TEST(DiagParse, TheThreeVerbs) {
  EXPECT_EQ(DIAG_PING,  diagParseCommand("ping"));
  EXPECT_EQ(DIAG_TRACE, diagParseCommand("trace"));
  EXPECT_EQ(DIAG_PEERS, diagParseCommand("peers"));
}

TEST(DiagParse, CaseAndSurroundingSpaceDoNotMatter) {
  EXPECT_EQ(DIAG_PING, diagParseCommand("  PING "));
  EXPECT_EQ(DIAG_PING, diagParseCommand("Ping\n"));
  EXPECT_EQ(DIAG_TRACE, diagParseCommand("\tTrAcE\r\n"));
}

// A bot that answers a prefix answers ordinary conversation as well, and then
// every chat message costs a transmission.
TEST(DiagParse, AVerbWithAnArgumentIsNotAVerb) {
  EXPECT_EQ(DIAG_UNKNOWN, diagParseCommand("ping me"));
  EXPECT_EQ(DIAG_UNKNOWN, diagParseCommand("pings"));
  EXPECT_EQ(DIAG_UNKNOWN, diagParseCommand("please ping"));
}

TEST(DiagParse, AnEmptyOrNullMessageIsUnknown) {
  EXPECT_EQ(DIAG_UNKNOWN, diagParseCommand(""));
  EXPECT_EQ(DIAG_UNKNOWN, diagParseCommand("   "));
  EXPECT_EQ(DIAG_UNKNOWN, diagParseCommand(nullptr));
}

TEST(DiagParse, TheUsageReplyNamesEveryVerbAndTheLoginRule) {
  std::string u = diagUsageText();
  EXPECT_TRUE(has(u, "ping"));
  EXPECT_TRUE(has(u, "trace"));
  EXPECT_TRUE(has(u, "peers"));
  EXPECT_TRUE(has(u, "login"));
}

// -------------------------------------------------------- the rate limits

TEST(DiagLimit, PingAllowsOneAndThenRefusesInsideTheBurstWindow) {
  DiagLimits lim;
  EXPECT_TRUE(lim.allowPing(1000));
  EXPECT_FALSE(lim.allowPing(1001));
  EXPECT_FALSE(lim.allowPing(1029));
}

// The window is fixed and not a slide. It opens again at exactly the width of
// the window, and not one second before.
TEST(DiagLimit, ThePingBurstWindowOpensAgainAtItsBoundary) {
  DiagLimits lim;
  EXPECT_TRUE(lim.allowPing(1000));
  EXPECT_FALSE(lim.allowPing(1029));   // 29 s: still inside
  EXPECT_TRUE(lim.allowPing(1030));    // 30 s: the window restarts
}

TEST(DiagLimit, TheTraceBurstWindowIsFiveMinutes) {
  DiagLimits lim;
  EXPECT_TRUE(lim.allowTrace(1000));
  EXPECT_FALSE(lim.allowTrace(1299));
  EXPECT_TRUE(lim.allowTrace(1300));
}

TEST(DiagLimit, PingSpendsThirtyInADayAndThenStops) {
  DiagLimits lim;
  uint32_t t = 1000;
  for (int i = 0; i < 30; i++) { EXPECT_TRUE(lim.allowPing(t)) << i; t += 30; }
  EXPECT_FALSE(lim.allowPing(t));      // the day budget is gone
  EXPECT_FALSE(lim.allowPing(t + 30));
}

TEST(DiagLimit, TraceSpendsTwelveInADayAndThenStops) {
  DiagLimits lim;
  uint32_t t = 1000;
  for (int i = 0; i < 12; i++) { EXPECT_TRUE(lim.allowTrace(t)) << i; t += 300; }
  EXPECT_FALSE(lim.allowTrace(t));
}

// A request that the burst window refuses must not touch the day budget.
// Otherwise heavy traffic in one minute spends the whole day.
TEST(DiagLimit, ABurstRefusalDoesNotSpendTheDayBudget) {
  DiagLimits lim;
  EXPECT_TRUE(lim.allowPing(1000));
  for (int i = 0; i < 500; i++) lim.allowPing(1001);   // all refused by the burst window
  uint32_t t = 1030;
  for (int i = 0; i < 29; i++) { EXPECT_TRUE(lim.allowPing(t)) << i; t += 30; }
  EXPECT_FALSE(lim.allowPing(t));   // 30 of 30 spent, and not one more
}

TEST(DiagLimit, TheVerbsDoNotShareABudget) {
  DiagLimits lim;
  EXPECT_TRUE(lim.allowPing(1000));
  EXPECT_TRUE(lim.allowTrace(1000));
  EXPECT_TRUE(lim.allowOther(1000));
}

// ------------------------------------------------ ping: the route type

TEST(DiagPing, AFloodPacketReportsItsHopCountAndItsPathPrefixes) {
  DiagPingFacts f = pingFacts();
  uint8_t path[] = { 0xa1, 0xb2 };
  f.hops = 2; f.path = path;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "flood route")) << s;
  EXPECT_TRUE(has(s, "2 hops")) << s;
  EXPECT_TRUE(has(s, "via a1,b2")) << s;
}

TEST(DiagPing, AFloodPacketWithNoRelaysSaysSoInWords) {
  DiagPingFacts f = pingFacts();
  f.hops = 0;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "flood route")) << s;
  EXPECT_TRUE(has(s, "0 hops")) << s;
}

// The relays take themselves out of a direct path, so the packet arrives with
// none. Reporting "0 hops" here reads as "you are next door", which is the one
// answer this case cannot support.
TEST(DiagPing, ADirectPacketNeverReportsAHopCount) {
  DiagPingFacts f = pingFacts();
  f.route_flood = false;
  f.hops = 0;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "direct route")) << s;
  EXPECT_TRUE(has(s, "relays")) << s;
  EXPECT_FALSE(has(s, "hops")) << s;
}

TEST(DiagPing, ALongPathIsCutAndTheCutIsMarked) {
  DiagPingFacts f = pingFacts();
  uint8_t path[20];
  for (int i = 0; i < 20; i++) path[i] = (uint8_t)(0x10 + i);
  f.hops = 20; f.path = path;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "20 hops")) << s;
  EXPECT_TRUE(has(s, ",..")) << s;
  EXPECT_FALSE(has(s, "23")) << s;   // the 20th entry never appears
}

TEST(DiagPing, TwoByteHashesPrintAtTheirFullWidth) {
  DiagPingFacts f = pingFacts();
  uint8_t path[] = { 0xa1, 0xb2, 0xc3, 0xd4 };
  f.hops = 2; f.hash_size = 2; f.path = path;
  EXPECT_TRUE(has(ping(f), "via a1b2,c3d4")) << ping(f);
}

// ------------------------------------------- ping: whose coding rate it is

TEST(DiagPing, AReportedCodingRateIsLabelledAsTheSenders) {
  DiagPingFacts f = pingFacts();
  f.cr = 6; f.cr_observed = true;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "cr 4/6 sender")) << s;
  EXPECT_FALSE(has(s, "configured")) << s;
}

// _cr is 0 when the radio could not read the header. The number in the reply is
// then ours, and so is the airtime. The reply must say so.
TEST(DiagPing, AnUnknownCodingRateIsLabelledAsOurConfiguredOne) {
  DiagPingFacts f = pingFacts();
  f.cr = 5; f.cr_observed = false;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "cr 4/5 configured (ours)")) << s;
  EXPECT_FALSE(has(s, "sender")) << s;
}

TEST(DiagPing, NoCodingRateAtAllClaimsNoNumber) {
  DiagPingFacts f = pingFacts();
  f.cr = 0; f.cr_observed = false;
  std::string s = ping(f);
  EXPECT_TRUE(has(s, "cr unknown")) << s;
  EXPECT_FALSE(has(s, "4/")) << s;
}

// ------------------------------------------------- ping: the link figures

TEST(DiagPing, TheLinkFiguresAndTheHashAllAppear) {
  std::string s = ping(pingFacts());
  EXPECT_TRUE(has(s, "snr +7.5")) << s;
  EXPECT_TRUE(has(s, "rssi -92dBm")) << s;
  EXPECT_TRUE(has(s, "air 552ms")) << s;
  EXPECT_TRUE(has(s, "hash 3f9c2a1b")) << s;
}

TEST(DiagPing, ANegativeSnrKeepsItsSign) {
  DiagPingFacts f = pingFacts();
  f.snr4 = -50;
  EXPECT_TRUE(has(ping(f), "snr -12.5")) << ping(f);
}

// One text message holds MAX_TEXT_LEN characters, and composeMsgPacket() drops
// a longer one. So a reply that does not fit is a reply that nobody receives.
// The path is last, so a cut costs path entries and never the link figures.
TEST(DiagPing, TheWorstCaseReplyFitsAndKeepsTheLinkFigures) {
  DiagPingFacts f = pingFacts();
  uint8_t path[64 * 3];
  memset(path, 0xee, sizeof(path));
  f.hops = 63; f.hash_size = 3; f.path = path; f.cr = 0; f.cr_observed = false;
  f.airtime_ms = 999999; f.rssi = -140; f.snr4 = -120;
  char buf[161];   // MAX_TEXT_LEN + 1
  int n = diagFormatPing(buf, sizeof(buf), f);
  std::string s(buf);
  EXPECT_LE(n, 160) << s;
  EXPECT_EQ(strlen(buf), (size_t)n) << s;   // nothing was cut by snprintf itself
  EXPECT_TRUE(has(s, "snr -30.0")) << s;
  EXPECT_TRUE(has(s, "hash 3f9c2a1b")) << s;
  EXPECT_TRUE(has(s, ",..")) << s;
}

// ------------------------------------------------------ the trace round trip

// The route to the client is S -> R1 -> R2 -> C. The trace turns round at R2,
// so the entries are R1, R2, R1. The node after the last entry is S, and that
// is where onTraceRecv() fires. C is never in the list, so C needs no trace
// support at all.
TEST(DiagTrace, ThePathGoesOutAndComesBackWithoutTheClient) {
  uint8_t out_path[] = { 0xa1, 0xb2 };
  uint8_t path[64];
  uint8_t n = diagBuildTracePath(path, sizeof(path), out_path, 2, 1);
  ASSERT_EQ(3, n);
  EXPECT_EQ(0xa1, path[0]);
  EXPECT_EQ(0xb2, path[1]);
  EXPECT_EQ(0xa1, path[2]);
}

TEST(DiagTrace, OneRelayGivesOneEntry) {
  uint8_t out_path[] = { 0xa1 };
  uint8_t path[64];
  ASSERT_EQ(1, diagBuildTracePath(path, sizeof(path), out_path, 1, 1));
  EXPECT_EQ(0xa1, path[0]);
}

TEST(DiagTrace, FourRelaysGiveSevenEntriesInAMirror) {
  uint8_t out_path[] = { 1, 2, 3, 4 };
  uint8_t path[64];
  ASSERT_EQ(7, diagBuildTracePath(path, sizeof(path), out_path, 4, 1));
  const uint8_t want[] = { 1, 2, 3, 4, 3, 2, 1 };
  EXPECT_EQ(0, memcmp(path, want, 7));
}

TEST(DiagTrace, TwoByteEntriesKeepTheirWidthInBothDirections) {
  uint8_t out_path[] = { 0xa1, 0xa2, 0xb1, 0xb2 };
  uint8_t path[64];
  ASSERT_EQ(6, diagBuildTracePath(path, sizeof(path), out_path, 2, 2));
  const uint8_t want[] = { 0xa1, 0xa2, 0xb1, 0xb2, 0xa1, 0xa2 };
  EXPECT_EQ(0, memcmp(path, want, 6));
}

// Each node on the way appends one SNR byte to Packet::path, and that array
// holds MAX_PATH_SIZE bytes. So the round trip has a hard ceiling.
TEST(DiagTrace, ARoundTripThatOverflowsTheSnrArrayIsRefused) {
  uint8_t out_path[64];
  memset(out_path, 0x11, sizeof(out_path));
  uint8_t path[300];
  EXPECT_EQ(63, diagBuildTracePath(path, sizeof(path), out_path, 32, 1));   // the last that fits
  EXPECT_EQ(0, diagBuildTracePath(path, sizeof(path), out_path, 33, 1));    // 65 entries
}

// Nine bytes of tag, auth code and flags sit in front of the path, and the
// payload of a packet holds MAX_PACKET_PAYLOAD bytes in total.
TEST(DiagTrace, ARoundTripThatOverflowsThePayloadIsRefused) {
  uint8_t out_path[64 * 4];
  memset(out_path, 0x11, sizeof(out_path));
  uint8_t path[300];
  EXPECT_EQ(0, diagBuildTracePath(path, sizeof(path), out_path, 32, 4));   // 63 x 4 B
  EXPECT_EQ(172, diagBuildTracePath(path, sizeof(path), out_path, 22, 4)); // 43 x 4 B
}

TEST(DiagTrace, NoRelaysMeansNoPath) {
  uint8_t path[64];
  EXPECT_EQ(0, diagBuildTracePath(path, sizeof(path), path, 0, 1));
}

// A Packet encodes the path width as (top 2 bits + 1), so 1, 2, 3 or 4 bytes. A
// TRACE encodes it as (1 << lower 2 bits of flags), so 1, 2, 4 or 8. The two
// agree everywhere except at 3, which therefore has no trace at all.
TEST(DiagTrace, ThePathWidthMapsToTraceFlagsOrIsRefused) {
  EXPECT_EQ(0, diagTraceFlagsForWidth(1));
  EXPECT_EQ(1, diagTraceFlagsForWidth(2));
  EXPECT_EQ(2, diagTraceFlagsForWidth(4));
  EXPECT_EQ(0xFF, diagTraceFlagsForWidth(3));
}

// ------------------------------------------------------- the trace reply

TEST(DiagTrace, TheReplyGivesOneSnrForEachHopAndOneForUs) {
  uint8_t hashes[] = { 0xa1, 0xb2, 0xa1 };
  int8_t snrs[] = { 30, 13, 24 };   // +7.5, +3.2, +6.0
  char buf[300];
  diagFormatTrace(buf, sizeof(buf), hashes, 1, snrs, 3, 33, 2);
  std::string s(buf);
  EXPECT_TRUE(has(s, "a1 +7.5")) << s;
  EXPECT_TRUE(has(s, "b2 +3.2")) << s;
  EXPECT_TRUE(has(s, "a1 +6.0")) << s;
  EXPECT_TRUE(has(s, "us +8.2")) << s;
  EXPECT_TRUE(has(s, "2 relays")) << s;
}

// The tail names the round trip, so it must survive a cut. A reply that stops
// after the hops leaves the reader with no idea what the figures describe.
TEST(DiagTrace, ALongRoundTripIsCutAndKeepsItsTail) {
  uint8_t hashes[61 * 4];
  int8_t snrs[61];
  for (int i = 0; i < 61; i++) snrs[i] = -120;
  memset(hashes, 0xee, sizeof(hashes));
  char buf[161];   // MAX_TEXT_LEN + 1
  int n = diagFormatTrace(buf, sizeof(buf), hashes, 4, snrs, 61, -120, 31);
  std::string s(buf);
  EXPECT_LE(n, 160) << s;
  EXPECT_EQ(strlen(buf), (size_t)n) << s;
  EXPECT_TRUE(has(s, " ..,")) << s;
  EXPECT_TRUE(has(s, "31 relays")) << s;
  EXPECT_TRUE(has(s, "us -30.0")) << s;
}

TEST(DiagTrace, EveryRefusalSaysWhy) {
  EXPECT_TRUE(has(diagTraceRefusalText(DIAG_TRACE_NO_PATH), "ping"));
  EXPECT_TRUE(has(diagTraceRefusalText(DIAG_TRACE_ZERO_HOP), "next door"));
  EXPECT_TRUE(has(diagTraceRefusalText(DIAG_TRACE_TOO_LONG), "too long"));
  EXPECT_TRUE(has(diagTraceRefusalText(DIAG_TRACE_IN_FLIGHT), "already"));
  EXPECT_TRUE(has(diagTraceRefusalText(DIAG_TRACE_BAD_WIDTH), "3-byte"));
}

// ------------------------------------------------------------ peers

// Decision E. A chat slot carries no ACL, so nobody can hold a login on one and
// `peers` always refuses there. This is the case that matters, because a chat
// slot is what a diagnostic bot normally runs on.
TEST(DiagPeers, WithNoLoginTheCommandIsRefused) {
  EXPECT_FALSE(diagPeersAllowed(nullptr));
}

TEST(DiagPeers, AMemberWhoIsNotAnAdminIsRefused) {
  uint8_t guest = 0, read_only = 1, read_write = 2;
  EXPECT_FALSE(diagPeersAllowed(&guest));
  EXPECT_FALSE(diagPeersAllowed(&read_only));
  EXPECT_FALSE(diagPeersAllowed(&read_write));
}

TEST(DiagPeers, AnAdminPasses) {
  uint8_t admin = 3;
  EXPECT_TRUE(diagPeersAllowed(&admin));
}

// ClientInfo::permissions carries the role in its lower 2 bits, and the upper
// bits hold other grants. The gate must read the role and ignore the rest.
TEST(DiagPeers, TheUpperPermissionBitsDoNotGrantOrRemoveAdmin) {
  uint8_t admin_plus = 0xFC | 3;
  uint8_t guest_plus = 0xFC | 0;
  EXPECT_TRUE(diagPeersAllowed(&admin_plus));
  EXPECT_FALSE(diagPeersAllowed(&guest_plus));
}

// The refusal must name the reason, so that an operator knows what to grant.
TEST(DiagPeers, TheRefusalNamesTheLoginAndTheReason) {
  std::string s = diagPeersDeniedText();
  EXPECT_TRUE(has(s, "login")) << s;
  EXPECT_TRUE(has(s, "other nodes")) << s;
}

// A node seen only in the middle of somebody else's path has told us nothing
// about its own link to us. It must never get an rx count or an SNR.
TEST(DiagPeers, ARelayOnlySightingIsNotReportedAsHearingThem) {
  MeshObserver::PeerEntry e = blankPeer();
  e.relays = 42;
  e.direct_rx = 0;
  std::string s = peerLine(e);
  EXPECT_TRUE(has(s, "relay42")) << s;
  EXPECT_FALSE(has(s, "rx")) << s;
  EXPECT_FALSE(has(s, "snr")) << s;
}

TEST(DiagPeers, ADirectSightingCarriesItsCountAndItsMeanSnr) {
  MeshObserver::PeerEntry e = blankPeer();
  e.direct_rx = 12;
  e.snr4_sum = 30; e.snr_n = 1;
  std::string s = peerLine(e);
  EXPECT_TRUE(has(s, "rx12 snr+7.5")) << s;
}

TEST(DiagPeers, ADirectSightingWithNoSnrSaysSoRatherThanShowZero) {
  MeshObserver::PeerEntry e = blankPeer();
  e.direct_rx = 3;
  e.snr_n = 0;
  EXPECT_TRUE(has(peerLine(e), "rx3 snr?")) << peerLine(e);
}

// A 1-byte hash match is a 1-in-256 coincidence. It stays a separate count with
// a question mark, and it never becomes a yes.
TEST(DiagPeers, AOneByteMatchIsNeverReportedAsProofTheyHearUs) {
  MeshObserver::PeerEntry e = blankPeer();
  e.heard_us = 0;
  e.heard_us_1b = 7;
  std::string s = peerLine(e);
  EXPECT_TRUE(has(s, "hear-us:1b7?")) << s;
  EXPECT_FALSE(has(s, "yes")) << s;
}

TEST(DiagPeers, ProofAndCoincidenceStayApartWhenBothExist) {
  MeshObserver::PeerEntry e = blankPeer();
  e.heard_us = 3;
  e.heard_us_1b = 2;
  EXPECT_TRUE(has(peerLine(e), "hear-us:yes3+2?")) << peerLine(e);
}

TEST(DiagPeers, NoEvidenceEitherWayReadsAsNo) {
  MeshObserver::PeerEntry e = blankPeer();
  EXPECT_TRUE(has(peerLine(e), "hear-us:no")) << peerLine(e);
}

// A 1-byte table entry does not identify a node. The row shows it, because to
// hide it hides real traffic, but the row marks it.
TEST(DiagPeers, AOneByteEntryIsMarkedAsUnproven) {
  MeshObserver::PeerEntry e = blankPeer();
  e.width = 1;
  EXPECT_TRUE(has(peerLine(e), "!1B")) << peerLine(e);
  e.width = 2;
  EXPECT_FALSE(has(peerLine(e), "!1B")) << peerLine(e);
}

TEST(DiagPeers, AnUnknownHopCountIsAQuestionMarkAndNotAZero) {
  MeshObserver::PeerEntry e = blankPeer();
  e.min_hops = 0;
  std::string s = peerLine(e);
  EXPECT_TRUE(has(s, "h?")) << s;
  EXPECT_FALSE(has(s, "h0")) << s;
}

// The bot packs rows into a text message and stops when the next row does not
// fit. Three of the widest rows must still fit, or a peers reply costs more
// messages than the budget holds.
TEST(DiagPeers, ThreeOfTheWidestRowsFitInOneTextMessage) {
  MeshObserver::PeerEntry e = blankPeer();
  e.direct_rx = 99999; e.snr4_sum = -50; e.snr_n = 1;
  e.heard_us = 999; e.heard_us_1b = 999;
  e.width = 1;
  e.last_ms = 0;
  char buf[120];
  int n = diagFormatPeerLine(buf, sizeof(buf), e, 100000000);
  EXPECT_LE(n * 3 + 2, 160) << n << " " << buf;   // 2 = the line separators
}
