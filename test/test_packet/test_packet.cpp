#include <gtest/gtest.h>
#include <Packet.h>
#include <string.h>

using namespace mesh;

static Packet makePacket() {
  Packet p;
  p.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);
  p.setPathHashSizeAndCount(1, 3);
  p.path[0] = 0xAA; p.path[1] = 0xBB; p.path[2] = 0xCC;
  p.payload_len = 5;
  memcpy(p.payload, "hello", 5);
  return p;
}

// The three receive values must fit in the padding that the structure already
// has. If this test fails, the packet pool is larger than before. Check the
// order of the fields before you change the number here.
TEST(PacketLinkMetrics, StructStaysWithinItsExistingTailPadding) {
  EXPECT_EQ(sizeof(Packet), 262u);
}

TEST(PacketLinkMetrics, AFreshPacketReportsUnknownRatherThanStaleMetrics) {
  Packet p;
  EXPECT_EQ(p.getCodingRate(), 0);   // 0 means that nobody told us
  EXPECT_EQ(p.getRSSI(), 0);
  EXPECT_FLOAT_EQ(p.getSNR(), 0.0f);
}

// An int8_t cuts the value here. SF12 links often read below -128 dBm.
TEST(PacketLinkMetrics, RssiHoldsTheWholeSensitivityRange) {
  Packet p;
  p._rssi = -148;
  EXPECT_EQ(p.getRSSI(), -148);
  p._rssi = -5;
  EXPECT_EQ(p.getRSSI(), -5);
}

TEST(PacketLinkMetrics, CodingRateIsCarriedAsTheDenominator) {
  Packet p;
  for (uint8_t cr = 5; cr <= 8; cr++) {
    p._cr = cr;
    EXPECT_EQ(p.getCodingRate(), cr);
  }
}

// The values describe the frame that we received. They do not describe the
// contents of the packet. They must not go on the air. If they did, two nodes
// that forward the same packet would send different bytes. The dedup hashes
// later in the mesh would also differ.
TEST(PacketWireFormat, ReceiveMetricsAreNotSerialised) {
  uint8_t without[MAX_TRANS_UNIT + 1], with[MAX_TRANS_UNIT + 1];

  Packet a = makePacket();
  uint8_t len_a = a.writeTo(without);

  Packet b = makePacket();
  b._rssi = -119;
  b._snr = -37;
  b._cr = 8;
  uint8_t len_b = b.writeTo(with);

  EXPECT_EQ(len_a, len_b);
  EXPECT_EQ(0, memcmp(without, with, len_a));
}

TEST(PacketWireFormat, EncodedLengthStillMatchesGetRawLength) {
  uint8_t buf[MAX_TRANS_UNIT + 1];

  Packet p = makePacket();
  p._rssi = -100; p._snr = 20; p._cr = 7;
  EXPECT_EQ(p.writeTo(buf), p.getRawLength());

  Packet t = makePacket();
  t.header = ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);
  t.transport_codes[0] = 0x1234;
  t.transport_codes[1] = 0x5678;
  EXPECT_EQ(t.writeTo(buf), t.getRawLength());
}

// readFrom() builds a packet from bytes that never held these values. Thus it
// must keep what the caller already knows about the frame. It must not invent
// values.
TEST(PacketWireFormat, ReadFromLeavesReceiveMetricsUntouched) {
  uint8_t buf[MAX_TRANS_UNIT + 1];
  Packet src = makePacket();
  uint8_t len = src.writeTo(buf);

  Packet dest;
  dest._rssi = -90;
  dest._snr = 24;
  dest._cr = 5;
  ASSERT_TRUE(dest.readFrom(buf, len));

  EXPECT_EQ(dest.getRSSI(), -90);
  EXPECT_FLOAT_EQ(dest.getSNR(), 6.0f);
  EXPECT_EQ(dest.getCodingRate(), 5);

  EXPECT_EQ(dest.header, src.header);
  EXPECT_EQ(dest.path_len, src.path_len);
  EXPECT_EQ(dest.payload_len, src.payload_len);
  EXPECT_EQ(0, memcmp(dest.payload, src.payload, src.payload_len));
}

// The dedup tables use this hash. A node that heard a packet with a strong
// signal must calculate the same hash as a node that heard it weakly.
TEST(PacketWireFormat, PacketHashIgnoresReceiveMetrics) {
  uint8_t h1[MAX_HASH_SIZE], h2[MAX_HASH_SIZE];

  Packet a = makePacket();
  a.calculatePacketHash(h1);

  Packet b = makePacket();
  b._rssi = -140; b._snr = -60; b._cr = 8;
  b.calculatePacketHash(h2);

  EXPECT_EQ(0, memcmp(h1, h2, MAX_HASH_SIZE));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
