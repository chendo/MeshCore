// `peers` output. MeshObserver keeps two things apart that a careless renderer
// would merge, and merging them is the only way this command can lie:
//
//   * "we can hear them" comes from direct sightings only. A node seen
//     mid-path has said nothing about its link to us.
//   * "they can hear us" is only proven by a >=2-byte hash match. A 1-byte
//     match is a 1-in-256 coincidence.
//
// These tests exist to make either regression fail loudly.

#include <gtest/gtest.h>
#include <PeerReport.h>
#include <string>

namespace {
MeshObserver::PeerEntry blank() {
  MeshObserver::PeerEntry e;
  memset(&e, 0, sizeof(e));
  e.width = 3;
  e.hash[0] = 0xa1; e.hash[1] = 0xb2; e.hash[2] = 0xc3;
  return e;
}
std::string row(const MeshObserver::PeerEntry& e, uint32_t now = 100000) {
  char buf[200];
  formatPeerRow(buf, sizeof(buf), e, now);
  return std::string(buf);
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
}

// --------------------------------------------------- we hear them / we do not

TEST(PeerRow, ARelayOnlySightingIsNotReportedAsHearingThem) {
  MeshObserver::PeerEntry e = blank();
  e.relays = 42;          // seen in other nodes' paths
  e.direct_rx = 0;        // never the last hop
  std::string s = row(e);
  EXPECT_TRUE(has(s, "no (relayed x42)"));
  EXPECT_FALSE(has(s, "rx=")) << s;
  EXPECT_FALSE(has(s, "snr")) << s;
}

TEST(PeerRow, ADirectSightingReportsTheCountAndItsSnr) {
  MeshObserver::PeerEntry e = blank();
  e.direct_rx = 142;
  e.snr4_sum = 29;   // 7.25 dB * 4
  e.snr_n = 1;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "rx=142"));
  EXPECT_TRUE(has(s, "snr=+7.2")) << s;
}

TEST(PeerRow, NegativeSnrKeepsItsSign) {
  MeshObserver::PeerEntry e = blank();
  e.direct_rx = 3;
  e.snr4_sum = -50;   // -12.5 dB
  e.snr_n = 1;
  EXPECT_TRUE(has(row(e), "snr=-12.5"));
}

TEST(PeerRow, SnrIsAMeanOverDirectSightingsOnly) {
  MeshObserver::PeerEntry e = blank();
  e.direct_rx = 4;
  e.snr4_sum = 40;   // 4 readings summing to 10 dB*4 => mean +2.5 dB
  e.snr_n = 4;
  EXPECT_TRUE(has(row(e), "snr=+2.5"));
}

TEST(PeerRow, NoSnrIsPrintedWhenNoneWasMeasured) {
  MeshObserver::PeerEntry e = blank();
  e.direct_rx = 5;
  e.snr_n = 0;       // direct, but no usable SNR
  std::string s = row(e);
  EXPECT_TRUE(has(s, "rx=5"));
  EXPECT_TRUE(has(s, "snr=?")) << s;
  // No fabricated 0.0 dB: an unmeasured link must not read as a perfect one.
  EXPECT_FALSE(has(s, "+0.0")) << s;
}

// ---------------------------------------------- they hear us: proof vs. noise

TEST(PeerRow, AOneByteMatchIsNeverPrintedAsConfirmation) {
  MeshObserver::PeerEntry e = blank();
  e.heard_us = 0;
  e.heard_us_1b = 7;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "unproven x7")) << s;
  EXPECT_FALSE(has(s, "yes")) << s;
}

TEST(PeerRow, AWiderMatchIsConfirmation) {
  MeshObserver::PeerEntry e = blank();
  e.heard_us = 31;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "yes x31")) << s;
  EXPECT_FALSE(has(s, "unproven")) << s;
}

TEST(PeerRow, TheTwoTalliesAreShownSeparatelyWhenBothExist) {
  MeshObserver::PeerEntry e = blank();
  e.heard_us = 31;
  e.heard_us_1b = 4;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "yes x31")) << s;
  EXPECT_TRUE(has(s, "+4?")) << s;      // carried, flagged, not added to 31
  EXPECT_FALSE(has(s, "x35")) << s;     // never summed
}

TEST(PeerRow, NoEvidenceEitherWayReadsAsNo) {
  MeshObserver::PeerEntry e = blank();
  std::string s = row(e);
  EXPECT_TRUE(has(s, " no ")) << s;
  EXPECT_FALSE(has(s, "yes")) << s;
  EXPECT_FALSE(has(s, "unproven")) << s;
}

// ------------------------------------------------------------- hash width

TEST(PeerRow, AOneByteEntryIsMarkedAsUnproven) {
  MeshObserver::PeerEntry e = blank();
  e.width = 1;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "a1")) << s;
  EXPECT_FALSE(has(s, "a1b2")) << s;    // only the bytes actually known
  EXPECT_TRUE(has(s, "1-byte hash")) << s;
}

TEST(PeerRow, AWiderEntryCarriesNoSuchWarning) {
  MeshObserver::PeerEntry e = blank();
  e.width = 2;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "a1b2")) << s;
  EXPECT_FALSE(has(s, "a1b2c3")) << s;
  EXPECT_FALSE(has(s, "1-byte hash")) << s;
}

TEST(PeerRow, ThreeByteEntriesPrintAllThree) {
  EXPECT_TRUE(has(row(blank()), "a1b2c3"));
}

// ---------------------------------------------------------------- identity

TEST(PeerRow, AnUnadvertisedPeerStaysAnonymous) {
  EXPECT_TRUE(has(row(blank()), "?"));
}

TEST(PeerRow, AnAdvertisedNameIsShown) {
  MeshObserver::PeerEntry e = blank();
  strcpy(e.name, "Hilltop");
  EXPECT_TRUE(has(row(e), "Hilltop"));
}

TEST(PeerRow, AKnownPubkeyIsShownWhenThereIsNoName) {
  MeshObserver::PeerEntry e = blank();
  e.pub[0] = 0xde; e.pub[1] = 0xad; e.pub[2] = 0xbe; e.pub[3] = 0xef;
  std::string s = row(e);
  EXPECT_TRUE(has(s, "deadbeef")) << s;
}

TEST(PeerRow, AFullyPackedNameDoesNotRunPastItsField) {
  MeshObserver::PeerEntry e = blank();
  memset(e.name, 'z', sizeof(e.name));   // deliberately unterminated
  std::string s = row(e);
  EXPECT_EQ(std::string::npos, s.find(std::string(sizeof(e.name) + 1, 'z')));
}

// --------------------------------------------------------------- hop count

TEST(PeerRow, AnUnknownDistanceIsAQuestionMarkNotZero) {
  MeshObserver::PeerEntry e = blank();
  e.min_hops = 0;
  std::string s = row(e);
  EXPECT_NE(std::string::npos, s.find(" ?  "));
}

TEST(PeerRow, AKnownDistanceIsPrinted) {
  MeshObserver::PeerEntry e = blank();
  e.min_hops = 3;
  EXPECT_TRUE(has(row(e), " 3 "));
}

// ------------------------------------------------------------------- ages

TEST(PeerAge, ScalesFromSecondsToDays) {
  char b[16];
  peerFmtAge(b, sizeof(b), 12000);              EXPECT_STREQ("12s", b);
  peerFmtAge(b, sizeof(b), 99000);              EXPECT_STREQ("99s", b);
  peerFmtAge(b, sizeof(b), 300000);             EXPECT_STREQ("5m", b);
  peerFmtAge(b, sizeof(b), 7200UL * 1000UL);    EXPECT_STREQ("2h", b);
  peerFmtAge(b, sizeof(b), 200000UL * 1000UL);  EXPECT_STREQ("2d", b);
}

TEST(PeerRow, AgeIsWrapSafe) {
  MeshObserver::PeerEntry e = blank();
  e.last_ms = 0xFFFFF000u;          // seen just before millis() wrapped
  std::string s = row(e, 0x00001000u);
  EXPECT_TRUE(has(s, "8s")) << s;   // 8192 ms, not 49 days
}

// ---------------------------------------------------------------- summary

TEST(PeerSummary, ReportsOccupancyAndHashWidths) {
  MeshObserver obs;
  char buf[200];
  formatPeerSummary(buf, sizeof(buf), obs);
  std::string s(buf);
  EXPECT_TRUE(has(s, "peers: 0 of 48")) << s;
  // Widths are part of the summary because a table of 1-byte entries is a much
  // weaker picture than the same count at three.
  EXPECT_TRUE(has(s, "1B=")) << s;
  EXPECT_TRUE(has(s, "2B=")) << s;
  EXPECT_TRUE(has(s, "3B=")) << s;
  EXPECT_TRUE(has(s, "hear-us")) << s;
  EXPECT_TRUE(has(s, "refused")) << s;
}

TEST(PeerSummary, SeparatesConfirmedPeersFromTheTotal) {
  // "48 peers" and "48 peers that can hear us" are very different claims.
  MeshObserver obs;
  char buf[200];
  formatPeerSummary(buf, sizeof(buf), obs);
  EXPECT_TRUE(has(std::string(buf), "hear-us 0"));
}

TEST(PeerReport, TheColumnHeaderNamesBothDirections) {
  std::string h = peerColumnHeader();
  EXPECT_TRUE(has(h, "we-hear-them"));
  EXPECT_TRUE(has(h, "they-hear-us"));
}

TEST(PeerRow, FitsTheSerialReplyBuffer) {
  MeshObserver::PeerEntry e = blank();
  e.width = 1;
  e.direct_rx = 4294967295u;
  e.relays = 4294967295u;
  e.heard_us = 4294967295u;
  e.heard_us_1b = 4294967295u;
  e.snr4_sum = -2000; e.snr_n = 1;
  e.min_hops = 255;
  memset(e.name, 'w', sizeof(e.name));
  char buf[160];
  int n = formatPeerRow(buf, sizeof(buf), e, 0);
  EXPECT_LT(strlen(buf), sizeof(buf));
  EXPECT_GT(n, 0);
}

// PeerReport pulls in the Arduino mock via MeshObserver.h; the observer itself
// never reads the clock in these tests, but the symbol has to exist.
unsigned long g_fake_millis = 0;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
