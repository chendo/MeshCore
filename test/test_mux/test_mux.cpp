// MuxSerialInterface lets a local client use the app protocol of a chat
// identity. The comments below call this client "the web". At the same time the
// MeshCore phone app can connect to the same identity over TCP. The routing
// rules are not obvious, and they come from real failures. These tests hold
// them in place. A response goes back to the client that asked. An asynchronous
// push always goes to the phone. The code copies every message and every push
// for the web to read. The web reads the copy and does NOT empty the offline
// queue of the device.

#include <gtest/gtest.h>
#include <vector>
#include "helpers/esp32/MuxSerialInterface.h"

unsigned long g_fake_millis = 0;
FakeSerial Serial;

namespace {

// the app-protocol codes that the tests below use. See docs/companion_protocol.md.
constexpr uint8_t CMD_SEND_LOGIN        = 26;
constexpr uint8_t RESP_CODE_SENT        = 6;
constexpr uint8_t RESP_CODE_CONTACT     = 3;
constexpr uint8_t PUSH_LOGIN_SUCCESS    = 0x85;
constexpr uint8_t PUSH_LOGIN_FAIL       = 0x86;
constexpr uint8_t PUSH_MSG_WAITING      = 0x83;
constexpr uint8_t RESP_CONTACT_MSG_V3   = 16;

struct Mux {
  MuxSerialInterface m;
  Mux() { m.init(8 * 1024); }

  // This function replaces the mesh loop. It gives the next queued frame to the
  // mesh.
  size_t pump(uint8_t* dest) { return m.checkRecvFrame(dest); }
};

std::vector<std::vector<uint8_t>> splitWebResponse(const uint8_t* buf, int n) {
  std::vector<std::vector<uint8_t>> out;
  int o = 0;
  while (o + 2 <= n) {
    int len = buf[o] | (buf[o + 1] << 8);
    out.emplace_back(buf + o + 2, buf + o + 2 + len);
    o += 2 + len;
  }
  return out;
}

// ------------------------------------------------------------ basic routing

TEST(Mux, WebFrameReachesTheMeshAndItsResponseComesBackToTheWeb) {
  Mux mux;
  uint8_t req[] = {CMD_SEND_LOGIN, 0xAA};
  ASSERT_TRUE(mux.m.webStart(req, sizeof(req)));

  uint8_t got[MAX_FRAME_SIZE];
  ASSERT_EQ(2u, mux.pump(got));
  EXPECT_EQ(CMD_SEND_LOGIN, got[0]);

  uint8_t resp[] = {RESP_CODE_SENT, 1};
  mux.m.writeFrame(resp, sizeof(resp));

  uint8_t out[512];
  int n = mux.m.webFinish(out, sizeof(out));
  auto frames = splitWebResponse(out, n);
  ASSERT_EQ(1u, frames.size());
  EXPECT_EQ(RESP_CODE_SENT, frames[0][0]);
}

TEST(Mux, ResponseToATcpCommandGoesToTheAppNotTheWeb) {
  Mux mux;
  mux.m.tcp.connected = true;
  mux.m.tcp.inbound.push_back({CMD_SEND_LOGIN, 0x01});

  uint8_t got[MAX_FRAME_SIZE];
  ASSERT_EQ(2u, mux.pump(got));            // the frame came from TCP, so the app gets the replies

  uint8_t resp[] = {RESP_CODE_SENT, 0};
  mux.m.writeFrame(resp, sizeof(resp));
  ASSERT_EQ(1u, mux.m.tcp.written.size());
  EXPECT_EQ(RESP_CODE_SENT, mux.m.tcp.written[0][0]);
}

TEST(Mux, WebFramesTakePriorityOverQueuedTcpFrames) {
  Mux mux;
  mux.m.tcp.connected = true;
  mux.m.tcp.inbound.push_back({0x01});
  uint8_t web[] = {0x02};
  ASSERT_TRUE(mux.m.webStart(web, 1));

  uint8_t got[MAX_FRAME_SIZE];
  ASSERT_EQ(1u, mux.pump(got));
  EXPECT_EQ(0x02, got[0]) << "the pending web frame should be served first";
}

TEST(Mux, OnlyOneWebExchangeAtATime) {
  Mux mux;
  uint8_t a[] = {1};
  ASSERT_TRUE(mux.m.webStart(a, 1));
  EXPECT_FALSE(mux.m.webStart(a, 1)) << "a second exchange must not clobber the first";
}

// ------------------------------------------------------------- push routing

TEST(Mux, AsyncPushesAlwaysGoToTheAppEvenDuringAWebExchange) {
  Mux mux;
  mux.m.tcp.connected = true;
  uint8_t req[] = {CMD_SEND_LOGIN};
  ASSERT_TRUE(mux.m.webStart(req, 1));
  uint8_t got[MAX_FRAME_SIZE];
  mux.pump(got);

  uint8_t push[] = {PUSH_MSG_WAITING};
  mux.m.writeFrame(push, 1);

  ASSERT_EQ(1u, mux.m.tcp.written.size()) << "pushes belong to the phone app";
  EXPECT_EQ(PUSH_MSG_WAITING, mux.m.tcp.written[0][0]);

  uint8_t out[512];
  int n = mux.m.webFinish(out, sizeof(out));
  EXPECT_EQ(0, n) << "a push is not a response to the web's command";
}

TEST(Mux, PushesAreMirroredSoTheWebCanObserveThemWithNoAppConnected) {
  Mux mux;                                  // there is no TCP client at all
  uint8_t push[] = {PUSH_LOGIN_SUCCESS, 1, 2, 3};
  mux.m.writeFrame(push, sizeof(push));

  uint8_t out[512];
  int n = mux.m.archiveCopy(0, out, sizeof(out));
  ASSERT_GT(n, 0) << "otherwise a web-initiated login looks like silence";
  // an archive entry is [u32 seq][u16 len][frame]
  uint16_t len = out[4] | (out[5] << 8);
  EXPECT_EQ(PUSH_LOGIN_SUCCESS, out[6]);
  EXPECT_EQ(4, len);
}

// ------------------------------------------------------- non-consuming view

TEST(MuxArchive, MessagesAreMirroredAndReadingDoesNotConsumeThem) {
  Mux mux;
  uint8_t msg[] = {RESP_CONTACT_MSG_V3, 0, 0, 0, 0xDE, 0xAD};
  mux.m.writeFrame(msg, sizeof(msg));

  uint8_t a[512], b[512];
  int n1 = mux.m.archiveCopy(0, a, sizeof(a));
  int n2 = mux.m.archiveCopy(0, b, sizeof(b));
  ASSERT_GT(n1, 0);
  EXPECT_EQ(n1, n2) << "reading the mirror must not drain it";
  EXPECT_EQ(0, memcmp(a, b, n1));
}

TEST(MuxArchive, CursorReturnsOnlyNewerEntries) {
  Mux mux;
  uint8_t m1[] = {RESP_CONTACT_MSG_V3, 1};
  mux.m.writeFrame(m1, sizeof(m1));
  uint32_t after = mux.m.archiveSeq();

  uint8_t m2[] = {RESP_CONTACT_MSG_V3, 2};
  mux.m.writeFrame(m2, sizeof(m2));

  uint8_t out[512];
  int n = mux.m.archiveCopy(after, out, sizeof(out));
  ASSERT_GT(n, 0);
  EXPECT_EQ(2, out[7]) << "only the frame after the cursor should come back";
}

TEST(MuxArchive, PlainResponsesAreNotMirrored) {
  Mux mux;
  uint8_t resp[] = {RESP_CODE_CONTACT, 0};
  uint8_t req[] = {4};
  ASSERT_TRUE(mux.m.webStart(req, 1));
  uint8_t got[MAX_FRAME_SIZE];
  mux.pump(got);
  mux.m.writeFrame(resp, sizeof(resp));

  uint8_t out[512];
  EXPECT_EQ(0, mux.m.archiveCopy(0, out, sizeof(out)))
      << "the mirror is for messages and pushes, not contact-sync chatter";
}

// -------------------------------------------------- simulated room join flow

// A complete room join that the web starts. The web sends CMD_SEND_LOGIN. It
// gets RESP_CODE_SENT at once. The answer of the room comes later as an
// asynchronous push.
TEST(MuxRoomJoin, SuccessfulJoinIsAcknowledgedThenConfirmedViaTheMirror) {
  Mux mux;
  uint32_t before = mux.m.archiveSeq();

  uint8_t login[1 + 32 + 8];
  login[0] = CMD_SEND_LOGIN;
  memset(login + 1, 0x6D, 32);                 // the public key of the room
  memcpy(login + 33, "hunter22", 8);           // the password for the join
  ASSERT_TRUE(mux.m.webStart(login, sizeof(login)));

  uint8_t got[MAX_FRAME_SIZE];
  ASSERT_EQ(sizeof(login), mux.pump(got));
  EXPECT_EQ(CMD_SEND_LOGIN, got[0]);

  uint8_t sent[10] = {RESP_CODE_SENT, 0};      // "the login request went out"
  mux.m.writeFrame(sent, sizeof(sent));

  uint8_t out[512];
  int n = mux.m.webFinish(out, sizeof(out));
  auto frames = splitWebResponse(out, n);
  ASSERT_EQ(1u, frames.size());
  ASSERT_EQ(RESP_CODE_SENT, frames[0][0]);

  // Seconds later the room answers over the air.
  uint8_t success[] = {PUSH_LOGIN_SUCCESS, 0x01};
  mux.m.writeFrame(success, sizeof(success));

  int an = mux.m.archiveCopy(before, out, sizeof(out));
  ASSERT_GT(an, 0);
  EXPECT_EQ(PUSH_LOGIN_SUCCESS, out[6]) << "the web must be able to see the join succeeded";
}

TEST(MuxRoomJoin, RejectedJoinSurfacesTheFailurePush) {
  Mux mux;
  uint32_t before = mux.m.archiveSeq();
  uint8_t login[1 + 32 + 5];
  login[0] = CMD_SEND_LOGIN;
  memset(login + 1, 0x6D, 32);
  memcpy(login + 33, "wrong", 5);
  ASSERT_TRUE(mux.m.webStart(login, sizeof(login)));
  uint8_t got[MAX_FRAME_SIZE];
  mux.pump(got);
  uint8_t sent[10] = {RESP_CODE_SENT, 0};
  mux.m.writeFrame(sent, sizeof(sent));
  uint8_t out[512];
  mux.m.webFinish(out, sizeof(out));

  uint8_t fail[] = {PUSH_LOGIN_FAIL};
  mux.m.writeFrame(fail, 1);

  int an = mux.m.archiveCopy(before, out, sizeof(out));
  ASSERT_GT(an, 0);
  EXPECT_EQ(PUSH_LOGIN_FAIL, out[6]);
}

TEST(MuxRoomJoin, AppAndWebCanUseTheIdentityConcurrentlyWithoutCrosstalk) {
  Mux mux;
  mux.m.tcp.connected = true;

  // the phone app synchronises its contacts
  mux.m.tcp.inbound.push_back({4});
  uint8_t got[MAX_FRAME_SIZE];
  ASSERT_EQ(1u, mux.pump(got));
  uint8_t contact[] = {RESP_CODE_CONTACT, 0xAB};
  mux.m.writeFrame(contact, sizeof(contact));
  ASSERT_EQ(1u, mux.m.tcp.written.size());

  // the web joins a room in the same session
  uint8_t login[] = {CMD_SEND_LOGIN, 0x01};
  ASSERT_TRUE(mux.m.webStart(login, sizeof(login)));
  ASSERT_EQ(2u, mux.pump(got));
  uint8_t sent[10] = {RESP_CODE_SENT, 0};
  mux.m.writeFrame(sent, sizeof(sent));

  uint8_t out[512];
  int n = mux.m.webFinish(out, sizeof(out));
  auto frames = splitWebResponse(out, n);
  ASSERT_EQ(1u, frames.size());
  EXPECT_EQ(RESP_CODE_SENT, frames[0][0]);
  EXPECT_EQ(1u, mux.m.tcp.written.size()) << "the web's reply must not leak to the phone app";
}

TEST(MuxConnected, ReportsConnectedWhileTheWebIsActiveSoAsyncResultsArentDropped) {
  g_fake_millis = 5000;
  Mux mux;
  EXPECT_FALSE(mux.m.isConnected()) << "nothing is attached yet";

  uint8_t f[] = {1};
  mux.m.webStart(f, 1);
  EXPECT_TRUE(mux.m.isConnected())
      << "stock code gates trace/login results on isConnected()";

  g_fake_millis += 61000;            // the web has gone away
  EXPECT_FALSE(mux.m.isConnected()) << "the web claim should lapse";
}

TEST(MuxConnected, IsNotConnectedRightAfterBootWithNothingAttached) {
  g_fake_millis = 0;                 // millis() is near zero at start-up
  Mux mux;
  EXPECT_FALSE(mux.m.isConnected());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
