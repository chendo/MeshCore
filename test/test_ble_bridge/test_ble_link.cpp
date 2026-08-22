#include <gtest/gtest.h>

#include <vector>

#include "helpers/nrf52/BleLink.h"

// BleLink itself, built against the Bluefruit stand-in in test/mocks_ble_link,
// so the shipped BleLink.cpp is what runs here. The framing is where the faults
// of this transport have been: a header that straddles two writes, a sender
// that stopped in the middle of a frame, and a stream that must find its way
// back to a frame boundary. Each of those is a case below.

namespace BleStack {
/* BleLink reads the MTU once, in begin(), to size a write. The rest of
   BleStack needs a SoftDevice and is not linked here. */
uint16_t mtu() { return 247; }
}

static const uint8_t SYNC = 0xA5;

struct Delivered {
  std::vector<uint8_t> data;
  uint8_t idx;
};
static std::vector<Delivered> g_rx;

static void rx_handler(const uint8_t* data, uint16_t len, uint8_t idx) {
  g_rx.push_back(Delivered{std::vector<uint8_t>(data, data + len), idx});
}

static ble_gap_addr_t addrEndingIn(uint8_t last) {
  ble_gap_addr_t a;
  memset(&a, 0, sizeof(a));
  for (uint8_t i = 0; i < 5; i++) a.addr[i] = (uint8_t)(0x10 + i);
  a.addr[5] = last;
  return a;
}

/* We dial only a peer whose address sorts above ours, so give ourselves the
   lower one. See BleLink::weInitiateTo. */
static ble_gap_addr_t self_addr() { return addrEndingIn(0x01); }
static ble_gap_addr_t peer_addr() { return addrEndingIn(0x02); }

static std::vector<uint8_t> framed(const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> f;
  f.push_back(SYNC);
  f.push_back((uint8_t)(payload.size() & 0xFF));
  f.push_back((uint8_t)(payload.size() >> 8));
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

static std::vector<uint8_t> payloadOf(size_t len, uint8_t seed = 1) {
  std::vector<uint8_t> p(len);
  for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(seed + i * 7);
  return p;
}

/* The bridge's deny list, as BleLink sees it: one address, and a switch. */
static ble_gap_addr_t g_denied;
static bool g_deny_on = false;
static uint32_t g_refused = 0;

static bool allow_handler(const ble_gap_addr_t& addr) {
  if (g_deny_on && memcmp(addr.addr, g_denied.addr, 6) == 0) {
    g_refused++;
    return false;
  }
  return true;
}

class BleLinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    BleMock::reset();
    g_rx.clear();
    g_deny_on = false;
    g_refused = 0;
    memset(&g_denied, 0, sizeof(g_denied));
    link.begin(rx_handler, self_addr(), allow_handler);
  }
  void TearDown() override { link.end(); }

  /** Dial the peer and answer the dial, so link 0 is UP. Reports the handle. */
  uint16_t bringUpLink0() {
    link.notePeer(peer_addr());
    link.loop();
    return BleMock::completeDial();
  }

  void notify(const std::vector<uint8_t>& bytes) {
    BleMock::notifyFrom(0, bytes.data(), (uint16_t)bytes.size());
  }

  BleLink link;
};

/* ---- Reassembly --------------------------------------------------------- */

TEST_F(BleLinkTest, aHeaderSplitAcrossTwoWritesStillMakesAFrame) {
  bringUpLink0();
  std::vector<uint8_t> payload = payloadOf(30);
  std::vector<uint8_t> f = framed(payload);

  /* The split falls between the length bytes, which is the case that the first
     version of this code dropped in silence. */
  notify(std::vector<uint8_t>(f.begin(), f.begin() + 2));
  EXPECT_EQ(g_rx.size(), 0u);
  notify(std::vector<uint8_t>(f.begin() + 2, f.end()));

  ASSERT_EQ(g_rx.size(), 1u);
  EXPECT_EQ(g_rx[0].data, payload);
  EXPECT_EQ(g_rx[0].idx, 0);
}

TEST_F(BleLinkTest, aTruncatedFrameDoesNotSwallowTheNextGoodOne) {
  bringUpLink0();
  std::vector<uint8_t> good = payloadOf(20, 9);

  /* A sender that stopped in the middle. Nothing more ever comes for it. */
  std::vector<uint8_t> stub = framed(payloadOf(40));
  notify(std::vector<uint8_t>(stub.begin(), stub.begin() + 10));
  EXPECT_EQ(g_rx.size(), 0u);

  /* Past the staleness limit, the part frame is abandoned. Without that, its
     missing tail eats the header of everything that follows. */
  BleMock::advance(3001);
  notify(framed(good));

  ASSERT_EQ(g_rx.size(), 1u);
  EXPECT_EQ(g_rx[0].data, good);
}

TEST_F(BleLinkTest, aTruncatedFrameCostsOneFrameAndNoMore) {
  bringUpLink0();
  std::vector<uint8_t> good = payloadOf(20, 3);

  /* The same truncation with NO pause. The missing tail takes the frames behind
     it until the length runs out, and one misaligned frame reaches the handler,
     where the group tag rejects it. The SYNC hunt must then recover, and the
     cost must stop there. */
  std::vector<uint8_t> stub = framed(payloadOf(40));
  notify(std::vector<uint8_t>(stub.begin(), stub.begin() + 10));
  notify(framed(payloadOf(20, 55)));
  notify(framed(payloadOf(20, 77)));
  notify(framed(good));

  ASSERT_EQ(g_rx.size(), 2u);
  EXPECT_NE(g_rx[0].data, good);             // the misaligned one
  EXPECT_EQ(g_rx[1].data, good);
}

TEST_F(BleLinkTest, aPayloadByteThatLooksLikeSyncDoesNotBecomeAFrame) {
  bringUpLink0();
  std::vector<uint8_t> good = payloadOf(24, 5);

  /* Misalign the stream, then offer bytes that a hunt could mistake for a
     header: SYNC with a zero length, and SYNC with a length above MAX_FRAME.
     Both are payload. If either one were taken, the good frame behind it would
     be consumed as its body. */
  std::vector<uint8_t> junk = {0x00, SYNC, 0x00, 0x00, 0x42, SYNC, 0x01, 0x02};
  notify(junk);
  EXPECT_EQ(g_rx.size(), 0u);
  notify(framed(good));

  ASSERT_EQ(g_rx.size(), 1u);
  EXPECT_EQ(g_rx[0].data, good);
}

/* ---- Teardown ----------------------------------------------------------- */

TEST_F(BleLinkTest, aReconnectDoesNotResumeAPartlyDrainedQueue) {
  bringUpLink0();

  std::vector<uint8_t> payload = payloadOf(64);
  ASSERT_EQ(link.send(payload.data(), (uint16_t)payload.size()), 1);

  /* Credits run out part way through the frame, which is ordinary. */
  BleMock::credits = 10;
  link.loop();
  ASSERT_EQ(BleMock::sink[0].size(), 10u);

  /* The peer goes quiet and the idle check drops the link. The same pass then
     dials again, because the backoff has long expired. */
  BleMock::advance(61000);
  link.loop();

  ble_gap_addr_t a;
  bool up;
  int8_t rssi;
  uint32_t sent, recv, drops, queued = 99;
  ASSERT_TRUE(link.getLink(0, a, up, rssi, sent, recv, drops, nullptr, &queued));
  EXPECT_FALSE(up);
  EXPECT_EQ(queued, 0u);                    // the frame belongs to the old link

  /* The peer comes back. Nothing may go out, because nothing is queued. A frame
     that resumed from its old offset would put a headerless tail on the wire,
     and a tail that carries a plausible SYNC and length builds a frame that
     fails the group tag. That count is what tells a foreign secret from a
     framing fault, so it must stay honest. */
  BleMock::clearSinks();
  BleMock::credits = -1;
  BleMock::completeDial();
  link.loop();
  EXPECT_TRUE(BleMock::sink[0].empty());
}

/* ---- The inbound peer --------------------------------------------------- */

/* A peer that dials US. It holds the single inbound slot, so every rule that
   protects an outward slot has to protect this one too. */
static ble_gap_addr_t inbound_addr() { return addrEndingIn(0x09); }

/** Open a connection and send one good frame over it, which adopts the peer. */
static uint16_t bringUpInbound(const ble_gap_addr_t& peer) {
  uint16_t h = BleMock::openConn(peer);
  std::vector<uint8_t> f = framed(payloadOf(16));
  BleMock::writeFrom(h, f.data(), (uint16_t)f.size());
  return h;
}

TEST_F(BleLinkTest, anInboundPeerThatGoesQuietLosesTheSlot) {
  bringUpInbound(inbound_addr());
  link.markAuthed(BleLink::INBOUND_LINK);

  ble_gap_addr_t a;
  ASSERT_TRUE(link.getInboundAddr(a));

  /* Authenticated, and then silent. The heartbeat is 15s, so a minute of
     nothing is a dead peer and not a lull. */
  BleMock::advance(61000);
  link.loop();
  EXPECT_FALSE(link.getInboundAddr(a));
  EXPECT_EQ(link.numUp(), 0);
}

TEST_F(BleLinkTest, anInboundPeerThatKeepsTalkingKeepsTheSlot) {
  uint16_t h = bringUpInbound(inbound_addr());
  link.markAuthed(BleLink::INBOUND_LINK);

  for (int i = 0; i < 6; i++) {
    BleMock::advance(15000);                 // one heartbeat interval
    std::vector<uint8_t> f = framed(payloadOf(16));
    BleMock::writeFrom(h, f.data(), (uint16_t)f.size());
    link.loop();
  }

  ble_gap_addr_t a;
  EXPECT_TRUE(link.getInboundAddr(a));
}

TEST_F(BleLinkTest, theInboundReceiveAgeIsVisible) {
  uint16_t h = BleMock::openConn(inbound_addr());

  /* A write adopts the peer, but no frame has completed yet. */
  const uint8_t junk[2] = {0x00, 0x00};
  BleMock::writeFrom(h, junk, 2);

  ble_gap_addr_t a;
  uint32_t age = 0, queued = 99;
  ASSERT_TRUE(link.getInboundAddr(a, &age, &queued));
  EXPECT_EQ(age, 0xFFFFFFFFu);
  EXPECT_EQ(queued, 0u);

  std::vector<uint8_t> f = framed(payloadOf(16));
  BleMock::writeFrom(h, f.data(), (uint16_t)f.size());
  BleMock::advance(5000);
  ASSERT_TRUE(link.getInboundAddr(a, &age, nullptr));
  EXPECT_EQ(age, 5u);
}

TEST_F(BleLinkTest, aWriteFromADroppedPeerDoesNotBuyANewWindow) {
  uint16_t h = bringUpInbound(inbound_addr());
  link.dropLink(BleLink::INBOUND_LINK);

  ble_gap_addr_t a;
  ASSERT_FALSE(link.getInboundAddr(a));

  /* The disconnect is asynchronous, so more writes from that peer still
     arrive. They must not adopt it again with a fresh grace window. */
  std::vector<uint8_t> f = framed(payloadOf(16));
  BleMock::writeFrom(h, f.data(), (uint16_t)f.size());
  EXPECT_FALSE(link.getInboundAddr(a));

  /* The hold is short on purpose: the SoftDevice reuses handles. */
  BleMock::advance(2001);
  BleMock::writeFrom(h, f.data(), (uint16_t)f.size());
  EXPECT_TRUE(link.getInboundAddr(a));
}

TEST_F(BleLinkTest, aDeniedPeerThatDialsInIsRefused) {
  g_denied = inbound_addr();
  g_deny_on = true;

  uint16_t h = BleMock::openConn(inbound_addr());
  std::vector<uint8_t> f = framed(payloadOf(16));
  BleMock::writeFrom(h, f.data(), (uint16_t)f.size());

  ble_gap_addr_t a;
  EXPECT_FALSE(link.getInboundAddr(a));
  EXPECT_EQ(g_refused, 1u);
  EXPECT_FALSE(BleMock::conns[h].connected);   // disconnected at once
  EXPECT_EQ(g_rx.size(), 0u);                  // and it carried nothing
}

TEST_F(BleLinkTest, aPeerThatIsNotDeniedIsAdopted) {
  g_denied = addrEndingIn(0x77);
  g_deny_on = true;

  bringUpInbound(inbound_addr());

  ble_gap_addr_t a;
  EXPECT_TRUE(link.getInboundAddr(a));
  EXPECT_EQ(g_refused, 0u);
  EXPECT_EQ(g_rx.size(), 1u);
}

/* ---- A peer that connects and says nothing ------------------------------ */

/* There is exactly ONE inbound slot. A write used to be the only event that
   made an inbound peer known, so a peer that connected and stayed silent was
   never adopted, never timed out and never denied. It held that slot for as
   long as its radio stayed in range. BleLink::loop() now sweeps the peripheral
   connections, which needs no Bluefruit callback and thus cannot take one from
   the BLE CLI. */

TEST_F(BleLinkTest, aPeerThatConnectsAndNeverWritesLosesTheSlot) {
  uint16_t h = BleMock::openConn(inbound_addr());

  /* Not one byte from this peer. The sweep is what makes it known. */
  link.loop();
  ble_gap_addr_t a;
  ASSERT_TRUE(link.getInboundAddr(a));

  /* From here it meets the ordinary authentication grace, because a peer that
     writes nothing has certainly proved no group membership. */
  BleMock::advance(45001);
  link.loop();
  EXPECT_FALSE(link.getInboundAddr(a));
  EXPECT_FALSE(BleMock::conns[h].connected);
  EXPECT_EQ(link.numUp(), 0);
}

TEST_F(BleLinkTest, aPeerThatWritesInsideTheLimitKeepsTheSlot) {
  uint16_t h = BleMock::openConn(inbound_addr());
  link.loop();
  ble_gap_addr_t a;
  ASSERT_TRUE(link.getInboundAddr(a));

  /* The peer speaks well inside the grace, and its frame passes the group tag.
     The sweep must cost an honest peer nothing. */
  BleMock::advance(30000);
  std::vector<uint8_t> f = framed(payloadOf(16));
  BleMock::writeFrom(h, f.data(), (uint16_t)f.size());
  link.markAuthed(BleLink::INBOUND_LINK);
  link.loop();

  /* Past the grace, and still up, because the peer authenticated. */
  BleMock::advance(30000);
  link.loop();
  EXPECT_TRUE(link.getInboundAddr(a));
  EXPECT_TRUE(BleMock::conns[h].connected);
  EXPECT_EQ(g_rx.size(), 1u);
}

TEST_F(BleLinkTest, aPeerDroppedForSilenceIsNotAdoptedAgainAtOnce) {
  uint16_t h = BleMock::openConn(inbound_addr());
  link.loop();
  ble_gap_addr_t a;
  ASSERT_TRUE(link.getInboundAddr(a));

  BleMock::advance(45001);
  link.loop();
  ASSERT_FALSE(link.getInboundAddr(a));

  /* A disconnect is asynchronous, so the connection is still there on the next
     pass. Without the hold the sweep adopts it again and grants a fresh grace
     window, which is exactly the squat that the drop was for. */
  BleMock::disconnectInFlight(h);
  link.loop();
  EXPECT_FALSE(link.getInboundAddr(a));

  /* The hold is short on purpose: the SoftDevice reuses handles. */
  BleMock::advance(2001);
  link.loop();
  EXPECT_TRUE(link.getInboundAddr(a));
}

TEST_F(BleLinkTest, aPeerThatSaysNothingReachesTheDenyList) {
  BleMock::openConn(inbound_addr());
  link.loop();

  BleMock::advance(45001);
  link.loop();

  /* Silence for the whole grace is three heartbeat intervals with no frame, so
     the peer is no use to this bridge. Without the deny list it reconnects at
     once and holds the single slot for another full window, and the disconnect
     buys nothing. */
  ble_gap_addr_t failed;
  ASSERT_TRUE(link.takeAuthFailure(failed));
  EXPECT_EQ(memcmp(failed.addr, inbound_addr().addr, 6), 0);
  EXPECT_FALSE(link.takeAuthFailure(failed));      // one drop, one report
}

TEST_F(BleLinkTest, theCliConnectionIsNotTakenForTheInboundPeer) {
  /* The CLI needs MITM encryption before it carries a byte, so its connection
     is secured and bonded. A bridge peer never pairs. That is how the sweep
     tells them apart, and the CLI must keep its slot whatever happens. */
  uint16_t cli = BleMock::openConn(addrEndingIn(0x55));
  BleMock::pair(cli);
  uint16_t peer = BleMock::openConn(inbound_addr());

  link.loop();
  ble_gap_addr_t a;
  ASSERT_TRUE(link.getInboundAddr(a));
  EXPECT_EQ(memcmp(a.addr, inbound_addr().addr, 6), 0);

  BleMock::advance(45001);
  link.loop();
  EXPECT_TRUE(BleMock::conns[cli].connected);
  EXPECT_FALSE(BleMock::conns[peer].connected);
}
