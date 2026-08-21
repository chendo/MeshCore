#include <gtest/gtest.h>

#include "helpers/bridges/BleBridgeFrame.h"

// The BLE bridge needs a SoftDevice, so BleStack, BleDiscovery, BleLink and
// BLEBridge itself cannot run here. Everything that does NOT need the radio
// lives in BleBridgeFrame.h, and this file covers it: the frame layout and its
// HMAC tag, the beacon record and its group marker, the deny list, and the
// oversize guard with its counter.
//
// The SHA-256 under this is the real one (test/mocks_ble), because a tag test
// against a no-op HMAC passes whatever the code does.

using namespace BleBridgeFrame;

static const char* SECRET = "LVSITANOS";
static const char* OTHER_SECRET = "different";

static FrameCodec ours() {
  FrameCodec c;
  c.setSecret(SECRET);
  return c;
}

// A stand-in for a serialised mesh packet. The content does not matter; only
// its length and the fact that the tag covers every byte of it.
static void fillPacket(uint8_t* buf, size_t len, uint8_t seed = 1) {
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(seed + i * 7);
}

/* ---- The frame ---------------------------------------------------------- */

TEST(BleFrame, buildsAndParsesADataFrame) {
  FrameCodec c = ours();
  uint8_t packet[40];
  fillPacket(packet, sizeof(packet));

  uint8_t frame[MAX_FRAME];
  size_t len = c.buildData(frame, 0x1234, 0xDEADBEEF, packet, sizeof(packet));
  ASSERT_EQ(len, HEADER_SIZE + sizeof(packet) + TAG_SIZE);
  EXPECT_EQ(frame[0], FRAME_VERSION);

  const uint8_t* out = nullptr;
  size_t out_len = 0;
  uint16_t seq = 0;
  uint32_t ts = 0;
  EXPECT_EQ(c.parse(frame, len, &out, &out_len, &seq, &ts), PARSE_DATA);
  EXPECT_EQ(seq, 0x1234);
  EXPECT_EQ(ts, 0xDEADBEEFu);
  ASSERT_EQ(out_len, sizeof(packet));
  EXPECT_EQ(memcmp(out, packet, sizeof(packet)), 0);
}

TEST(BleFrame, buildsAndParsesAHeartbeat) {
  FrameCodec c = ours();
  uint8_t frame[MAX_FRAME];
  size_t len = c.buildHeartbeat(frame, 7, 1000);
  ASSERT_EQ(len, HEADER_SIZE + TAG_SIZE);
  EXPECT_EQ(frame[0], FRAME_HEARTBEAT);

  uint16_t seq = 0;
  uint32_t ts = 0;
  EXPECT_EQ(c.parse(frame, len, nullptr, nullptr, &seq, &ts), PARSE_HEARTBEAT);
  EXPECT_EQ(seq, 7);
  EXPECT_EQ(ts, 1000u);
}

// The group secret is what keeps two neighbouring bridge groups apart. This is
// the role that ESPNowBridge gives to a checksum over its XORed payload.
TEST(BleFrame, aDifferentSecretFailsTheTag) {
  FrameCodec mine = ours();
  FrameCodec theirs;
  theirs.setSecret(OTHER_SECRET);

  uint8_t packet[24];
  fillPacket(packet, sizeof(packet));
  uint8_t frame[MAX_FRAME];
  size_t len = theirs.buildData(frame, 1, 1, packet, sizeof(packet));

  EXPECT_EQ(mine.parse(frame, len), PARSE_BAD_TAG);
  EXPECT_EQ(theirs.parse(frame, len), PARSE_DATA);
}

// The tag covers the version, the sequence, the timestamp and the packet, so no
// field is malleable.
TEST(BleFrame, everyByteBeforeTheTagIsCovered) {
  FrameCodec c = ours();
  uint8_t packet[16];
  fillPacket(packet, sizeof(packet));
  uint8_t frame[MAX_FRAME];
  size_t len = c.buildData(frame, 0x0102, 0x03040506, packet, sizeof(packet));
  ASSERT_EQ(c.parse(frame, len), PARSE_DATA);

  for (size_t i = 0; i < len - TAG_SIZE; i++) {
    uint8_t saved = frame[i];
    frame[i] = (uint8_t)(saved ^ 0x01);
    ParseResult r = c.parse(frame, len);
    // Byte 0 is the version, so a flipped bit there reads as another protocol
    // rather than as a bad tag. Everywhere else the tag is what catches it.
    EXPECT_TRUE(r == PARSE_BAD_TAG || (i == 0 && r == PARSE_FOREIGN))
        << "byte " << i << " is not covered";
    frame[i] = saved;
  }
}

TEST(BleFrame, rejectsAnotherProtocol) {
  FrameCodec c = ours();
  uint8_t frame[MAX_FRAME] = { 0 };

  // Too short to hold a header and a tag.
  EXPECT_EQ(c.parse(frame, HEADER_SIZE + TAG_SIZE - 1), PARSE_FOREIGN);
  // A version that we do not speak.
  frame[0] = 0x7F;
  EXPECT_EQ(c.parse(frame, HEADER_SIZE + TAG_SIZE + 4), PARSE_FOREIGN);
  // Our version, but with no packet at all: a data frame carries at least one
  // byte, and a heartbeat has its own type.
  frame[0] = FRAME_VERSION;
  EXPECT_EQ(c.parse(frame, HEADER_SIZE + TAG_SIZE), PARSE_FOREIGN);
}

/* ---- The oversize guard ------------------------------------------------- */

// MAX_FRAME is 256, and the header and the tag take 15, so a packet of 241
// bytes is the largest that fits. A real MeshCore packet is far below that, but
// the guard stays and it counts, because BRIDGE_DEBUG is off in a normal build
// and the ported version recorded an oversize packet nowhere at all.
TEST(BleFrame, theLargestPacketThatFitsIs241Bytes) {
  FrameCodec c = ours();
  EXPECT_EQ(MAX_PAYLOAD_SIZE, 241u);

  uint8_t packet[300];
  fillPacket(packet, sizeof(packet));
  uint8_t frame[MAX_FRAME];

  size_t len = c.buildData(frame, 1, 1, packet, MAX_PAYLOAD_SIZE);
  EXPECT_EQ(len, MAX_FRAME);
  EXPECT_EQ(c.parse(frame, len), PARSE_DATA);
  EXPECT_EQ(c.numTxOversize(), 0u);
}

TEST(BleFrame, countsAPacketThatIsTooLarge) {
  FrameCodec c = ours();
  uint8_t packet[300];
  fillPacket(packet, sizeof(packet));
  uint8_t frame[MAX_FRAME];

  EXPECT_EQ(c.buildData(frame, 1, 1, packet, MAX_PAYLOAD_SIZE + 1), 0u);
  EXPECT_EQ(c.numTxOversize(), 1u);
  // 254 is the theoretical maximum of Packet::writeTo(): header, transport
  // codes, path length, a 64-hop path and a 184-byte payload.
  EXPECT_EQ(c.buildData(frame, 2, 1, packet, 254), 0u);
  EXPECT_EQ(c.numTxOversize(), 2u);
}

/* ---- The group marker --------------------------------------------------- */

TEST(BleMarker, isDerivedConsistentlyFromTheSecret) {
  FrameCodec a, b;
  a.setSecret(SECRET);
  b.setSecret(SECRET);
  EXPECT_EQ(a.groupMarker(), b.groupMarker());

  FrameCodec other;
  other.setSecret(OTHER_SECRET);
  EXPECT_NE(a.groupMarker(), other.groupMarker());
}

// The marker goes out in the clear, so it must not be the start of a frame tag.
// A separate label is what keeps them apart.
TEST(BleMarker, isNotTheStartOfAFrameTag) {
  FrameCodec c = ours();
  uint8_t frame[MAX_FRAME];
  size_t len = c.buildHeartbeat(frame, 0, 0);
  const uint8_t* tag = &frame[len - TAG_SIZE];
  uint16_t from_tag = (uint16_t)tag[0] | ((uint16_t)tag[1] << 8);
  EXPECT_NE(c.groupMarker(), from_tag);
}

/* ---- The beacon --------------------------------------------------------- */

// One advert: flags, our manufacturer record, and a name.
static size_t buildAdvert(uint8_t* ad, uint16_t company, uint16_t marker,
                          uint8_t batt_dv, const char* name) {
  size_t i = 0;
  ad[i++] = 2; ad[i++] = 0x01; ad[i++] = 0x04;              // Flags
  ad[i++] = BEACON_LEN + 1; ad[i++] = 0xFF;
  buildBeaconRecord(&ad[i], company, marker, batt_dv); i += BEACON_LEN;
  size_t nlen = strlen(name);
  ad[i++] = (uint8_t)(nlen + 1); ad[i++] = 0x09;            // Complete Local Name
  memcpy(&ad[i], name, nlen); i += nlen;
  return i;
}

TEST(BleBeacon, findsAPeerOfOurGroup) {
  FrameCodec c = ours();
  uint8_t ad[31];
  size_t len = buildAdvert(ad, 0xFFFF, c.groupMarker(), 37, "MeshCore-A");

  uint8_t batt = 0;
  EXPECT_EQ(parseBeacon(ad, len, 0xFFFF, c.groupMarker(), &batt), BEACON_MATCH);
  EXPECT_EQ(batt, 37);
}

// A node with a different secret is in radio range, speaks the same protocol,
// and must NOT be dialled: there are only three central slots.
TEST(BleBeacon, doesNotDialAForeignMarker) {
  FrameCodec mine = ours();
  FrameCodec theirs;
  theirs.setSecret(OTHER_SECRET);

  uint8_t ad[31];
  size_t len = buildAdvert(ad, 0xFFFF, theirs.groupMarker(), 37, "MeshCore-B");

  EXPECT_EQ(parseBeacon(ad, len, 0xFFFF, mine.groupMarker()), BEACON_FOREIGN);
  EXPECT_EQ(parseBeacon(ad, len, 0xFFFF, theirs.groupMarker()), BEACON_MATCH);
}

// 0xFFFF is the shared development company ID of the Bluetooth SIG, so any
// other beacon can reach the record check. A record of the wrong shape or the
// wrong version is foreign, and it must not crash the walk.
TEST(BleBeacon, treatsAShortOrOldRecordAsForeign) {
  FrameCodec c = ours();
  uint8_t ad[31];
  size_t i = 0;
  ad[i++] = 4; ad[i++] = 0xFF; ad[i++] = 0xFF; ad[i++] = 0xFF; ad[i++] = 0x01;
  EXPECT_EQ(parseBeacon(ad, i, 0xFFFF, c.groupMarker()), BEACON_FOREIGN);

  i = buildAdvert(ad, 0xFFFF, c.groupMarker(), 37, "MeshCore-A");
  // Flags(3) + AD length(1) + AD type(1) + company(2), so the record version
  // byte is at offset 7.
  ad[7] = 0x02;                                   // a record version we do not speak
  EXPECT_EQ(parseBeacon(ad, i, 0xFFFF, c.groupMarker()), BEACON_FOREIGN);
}

TEST(BleBeacon, ignoresAnAdvertWithNoRecordOfOurs) {
  FrameCodec c = ours();
  uint8_t ad[31];
  size_t i = 0;
  ad[i++] = 2; ad[i++] = 0x01; ad[i++] = 0x04;              // Flags only
  ad[i++] = 5; ad[i++] = 0x09; ad[i++] = 'a'; ad[i++] = 'b'; ad[i++] = 'c'; ad[i++] = 'd';
  EXPECT_EQ(parseBeacon(ad, i, 0xFFFF, c.groupMarker()), BEACON_NONE);

  // A malformed length must end the walk and not read past the buffer.
  ad[0] = 200;
  EXPECT_EQ(parseBeacon(ad, i, 0xFFFF, c.groupMarker()), BEACON_NONE);
  EXPECT_EQ(parseBeacon(nullptr, 0, 0xFFFF, c.groupMarker()), BEACON_NONE);
}

/* ---- The deny list ------------------------------------------------------ */

static const uint8_t ADDR_A[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t ADDR_B[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };

TEST(BleDeny, refusesAnAddressAndThenAllowsItAgain) {
  BleDenyList deny;
  const uint32_t T0 = 100000;

  EXPECT_FALSE(deny.isDenied(ADDR_A, T0));
  deny.add(ADDR_A, T0);
  EXPECT_TRUE(deny.isDenied(ADDR_A, T0));
  EXPECT_TRUE(deny.isDenied(ADDR_A, T0 + BleDenyList::DENY_MS - 1));
  EXPECT_FALSE(deny.isDenied(ADDR_A, T0 + BleDenyList::DENY_MS));
  EXPECT_EQ(deny.numAdded(), 1u);
}

TEST(BleDeny, refusesOnlyTheAddressThatFailed) {
  BleDenyList deny;
  deny.add(ADDR_A, 0);
  EXPECT_TRUE(deny.isDenied(ADDR_A, 0));
  EXPECT_FALSE(deny.isDenied(ADDR_B, 0));
  EXPECT_EQ(deny.size(0), 1);
}

// A repeat offender restarts its own period and does not consume a second slot.
TEST(BleDeny, restartsThePeriodOnARepeat) {
  BleDenyList deny;
  deny.add(ADDR_A, 0);
  deny.add(ADDR_A, 1000);
  EXPECT_EQ(deny.size(1000), 1);
  EXPECT_EQ(deny.numAdded(), 2u);
  EXPECT_TRUE(deny.isDenied(ADDR_A, BleDenyList::DENY_MS));
  EXPECT_FALSE(deny.isDenied(ADDR_A, 1000 + BleDenyList::DENY_MS));
}

// The list is full only when eight distinct addresses failed inside one period.
// The entry that expires first is the one to lose, so the newest attackers stay
// listed and the three central slots stay protected.
TEST(BleDeny, evictsTheEntryThatExpiresFirst) {
  BleDenyList deny;
  uint8_t addr[6] = { 0xAA, 0, 0, 0, 0, 0 };
  for (uint8_t i = 0; i < BleDenyList::CAPACITY; i++) {
    addr[5] = i;
    deny.add(addr, (uint32_t)(i * 10));
  }
  EXPECT_EQ(deny.size(100), (uint8_t)BleDenyList::CAPACITY);

  uint8_t oldest[6] = { 0xAA, 0, 0, 0, 0, 0 };
  addr[5] = 99;
  deny.add(addr, 1000);
  EXPECT_FALSE(deny.isDenied(oldest, 1000));
  EXPECT_TRUE(deny.isDenied(addr, 1000));
  EXPECT_EQ(deny.size(1000), (uint8_t)BleDenyList::CAPACITY);
}

// millis() wraps at about 49 days, and the comparison is signed so that a
// deadline on the far side of the wrap still reads as the future.
TEST(BleDeny, survivesTheMillisWrap) {
  BleDenyList deny;
  const uint32_t NEAR_WRAP = 0xFFFFFFFFu - 1000;
  deny.add(ADDR_A, NEAR_WRAP);
  EXPECT_TRUE(deny.isDenied(ADDR_A, NEAR_WRAP));
  EXPECT_TRUE(deny.isDenied(ADDR_A, (uint32_t)(NEAR_WRAP + 2000)));   // wrapped
  EXPECT_FALSE(deny.isDenied(ADDR_A, (uint32_t)(NEAR_WRAP + BleDenyList::DENY_MS)));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
