// These tests cover the room protocol, minus the radio. They are the port of
// the delivery logic in examples/simple_room_server, so a change that alters
// behaviour must break a test here and not on hardware.
//
// The four rules that these tests pin down:
//
//   SYNC-SINCE   A client receives a post whose timestamp is AFTER its own
//                sync point, and nothing else. The sync point moves only when
//                the client acknowledges a push.
//   THE RING     The buffer holds MAX_UNSYNCED_POSTS posts. Post N+1 overwrites
//                the oldest slot. A client that is late by more than the depth
//                of the ring loses those posts for good. That is what makes
//                this a DELIVERY BUFFER and not a history.
//   THE AUTHOR   A post never goes back to the client that wrote it.
//   THE RETRY    A push with no ACK is sent again, up to ROOM_MAX_PUSH_FAILURES
//                attempts. Then the client is frozen until it talks again.

#include <gtest/gtest.h>
#include <RoomSync.h>

namespace {

// A public key that is easy to read in a failure message.
struct Key {
  uint8_t b[PUB_KEY_SIZE];
  explicit Key(uint8_t fill) { memset(b, fill, sizeof(b)); }
};

const Key ALICE(0xA1), BOB(0xB2), CAROL(0xC3);

// Far enough past the newest post that the delay gate never hides one.
uint32_t ripe(uint32_t post_ts) { return post_ts + POST_SYNC_DELAY_SECS; }

}  // namespace

// -------------------------------------------------------------- sync-since

TEST(RoomSync, AClientWithNoSyncPointGetsTheOldestPostFirst) {
  PostRing ring;
  ring.add(ALICE.b, "one", 1000);
  ring.add(ALICE.b, "two", 1001);

  int idx = ring.nextForClient(BOB.b, 0, ripe(1001));
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(ring.posts[idx].text, "one");
}

TEST(RoomSync, APostAtTheSyncPointIsAlreadySeen) {
  PostRing ring;
  ring.add(ALICE.b, "one", 1000);
  ring.add(ALICE.b, "two", 1001);

  // sync_since == 1000 means "I have post 1000". The test is strictly greater.
  int idx = ring.nextForClient(BOB.b, 1000, ripe(1001));
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(ring.posts[idx].text, "two");
}

TEST(RoomSync, ASyncPointPastEverythingLeavesNothingToSend) {
  PostRing ring;
  ring.add(ALICE.b, "one", 1000);
  ring.add(ALICE.b, "two", 1001);

  EXPECT_EQ(ring.nextForClient(BOB.b, 1001, ripe(1001)), -1);
}

TEST(RoomSync, AnEmptyBufferHasNothingToSend) {
  PostRing ring;
  EXPECT_EQ(ring.nextForClient(BOB.b, 0, 999999), -1);
}

// An unused slot has a timestamp of 0. It must never look like a post.
TEST(RoomSync, UnusedSlotsAreNeverOffered) {
  PostRing ring;
  ring.add(ALICE.b, "only", 1000);

  int idx = ring.nextForClient(BOB.b, 0, ripe(1000));
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(ring.posts[idx].text, "only");
  // and nothing after it
  EXPECT_EQ(ring.nextForClient(BOB.b, 1000, ripe(1000)), -1);
}

TEST(RoomSync, APostWaitsForTheSyncDelayBeforeItIsOffered) {
  PostRing ring;
  ring.add(ALICE.b, "fresh", 1000);

  EXPECT_EQ(ring.nextForClient(BOB.b, 0, 1000 + POST_SYNC_DELAY_SECS - 1), -1);
  EXPECT_GE(ring.nextForClient(BOB.b, 0, 1000 + POST_SYNC_DELAY_SECS), 0);
}

TEST(RoomSync, PostsAreOfferedOldestFirst) {
  PostRing ring;
  for (int i = 0; i < 5; i++) ring.add(ALICE.b, "x", 1000 + i);

  uint32_t sync = 0;
  for (int i = 0; i < 5; i++) {
    int idx = ring.nextForClient(BOB.b, sync, ripe(1004));
    ASSERT_GE(idx, 0) << "at step " << i;
    EXPECT_EQ(ring.posts[idx].post_timestamp, (uint32_t)(1000 + i));
    sync = ring.posts[idx].post_timestamp;   // as an ACK would move it
  }
  EXPECT_EQ(ring.nextForClient(BOB.b, sync, ripe(1004)), -1);
}

// -------------------------------------------------------------- the ring wrap

TEST(RoomSync, TheCursorWrapsBackToTheStart) {
  PostRing ring;
  for (int i = 0; i < MAX_UNSYNCED_POSTS; i++) ring.add(ALICE.b, "x", 1000 + i);
  EXPECT_EQ(ring.next_post_idx, 0);
  EXPECT_EQ(ring.num_posted, (uint16_t)MAX_UNSYNCED_POSTS);
}

TEST(RoomSync, PostNPlusOneOverwritesTheOldestSlot) {
  PostRing ring;
  for (int i = 0; i < MAX_UNSYNCED_POSTS; i++) ring.add(ALICE.b, "old", 1000 + i);
  ring.add(ALICE.b, "newest", 2000);

  EXPECT_EQ(ring.next_post_idx, 1);
  EXPECT_STREQ(ring.posts[0].text, "newest");
  EXPECT_EQ(ring.posts[0].post_timestamp, 2000u);
}

// This is the delivery-buffer property, stated as a test. A client that is away
// for longer than the depth of the ring loses those posts. A bigger ring widens
// the window. It never gives a new member a backlog.
TEST(RoomSync, AClientBehindByMoreThanTheRingDepthLosesThosePosts) {
  PostRing ring;
  for (int i = 0; i < MAX_UNSYNCED_POSTS; i++) ring.add(ALICE.b, "x", 1000 + i);
  // post 1000 is still the oldest one held
  int idx = ring.nextForClient(BOB.b, 0, ripe(2100));
  ASSERT_GE(idx, 0);
  EXPECT_EQ(ring.posts[idx].post_timestamp, 1000u);

  ring.add(ALICE.b, "pushes 1000 out", 2000);

  // The same client asks again. Post 1000 is gone, and nothing reports it.
  idx = ring.nextForClient(BOB.b, 0, ripe(2100));
  ASSERT_GE(idx, 0);
  EXPECT_EQ(ring.posts[idx].post_timestamp, 1001u);
}

TEST(RoomSync, AfterAWrapTheScanStillStartsAtTheOldestPost) {
  PostRing ring;
  for (int i = 0; i < MAX_UNSYNCED_POSTS + 3; i++) ring.add(ALICE.b, "x", 1000 + i);

  int idx = ring.nextForClient(BOB.b, 0, ripe(2000));
  ASSERT_GE(idx, 0);
  // the first three are overwritten, so 1003 is the oldest one held
  EXPECT_EQ(ring.posts[idx].post_timestamp, 1003u);
  EXPECT_EQ(idx, 3);
}

TEST(RoomSync, TheTextIsTruncatedAndStaysATerminatedString) {
  PostRing ring;
  char big[MAX_POST_TEXT_LEN + 50];
  memset(big, 'z', sizeof(big));
  big[sizeof(big) - 1] = 0;
  ring.add(ALICE.b, big, 1000);

  EXPECT_EQ((int)strlen(ring.posts[0].text), MAX_POST_TEXT_LEN);
}

// --------------------------------------------------------------- the author

TEST(RoomSync, APostIsNeverSentBackToItsAuthor) {
  PostRing ring;
  ring.add(ALICE.b, "mine", 1000);

  EXPECT_EQ(ring.nextForClient(ALICE.b, 0, ripe(1000)), -1);
  EXPECT_GE(ring.nextForClient(BOB.b, 0, ripe(1000)), 0);
}

TEST(RoomSync, AnAuthorStillGetsThePostsOfOtherPeople) {
  PostRing ring;
  ring.add(ALICE.b, "from alice", 1000);
  ring.add(BOB.b,   "from bob",   1001);
  ring.add(ALICE.b, "alice again", 1002);

  int idx = ring.nextForClient(ALICE.b, 0, ripe(1002));
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(ring.posts[idx].text, "from bob");

  // and nothing more, because the other two are hers
  EXPECT_EQ(ring.nextForClient(ALICE.b, 1001, ripe(1002)), -1);
}

TEST(RoomSync, TheAuthorTestUsesTheWholeKeyAndNotAPrefix) {
  PostRing ring;
  Key almost_alice(0xA1);
  almost_alice.b[PUB_KEY_SIZE - 1] = 0x00;   // differs in the last byte only
  ring.add(ALICE.b, "mine", 1000);

  EXPECT_GE(ring.nextForClient(almost_alice.b, 0, ripe(1000)), 0);
}

// --------------------------------------------------------- the unsynced count

TEST(RoomSync, TheUnsyncedCountSkipsThePostsOfTheClientItself) {
  PostRing ring;
  ring.add(ALICE.b, "a", 1000);
  ring.add(BOB.b,   "b", 1001);
  ring.add(CAROL.b, "c", 1002);

  EXPECT_EQ(ring.unsyncedCount(ALICE.b, 0), 2);
  EXPECT_EQ(ring.unsyncedCount(BOB.b, 0), 2);
  EXPECT_EQ(ring.unsyncedCount(BOB.b, 1001), 1);
  EXPECT_EQ(ring.unsyncedCount(BOB.b, 1002), 0);
}

// The count has no delay gate. It reports a post that is still too new to push.
// The client uses the number as a hint, so this matches upstream.
TEST(RoomSync, TheUnsyncedCountIgnoresTheSyncDelay) {
  PostRing ring;
  ring.add(ALICE.b, "fresh", 1000);
  EXPECT_EQ(ring.unsyncedCount(BOB.b, 0), 1);
  EXPECT_EQ(ring.nextForClient(BOB.b, 0, 1000), -1);
}

// ---------------------------------------------------------------- the retry

TEST(RoomRetry, AClientThatOwesAnAckIsSkipped) {
  EXPECT_FALSE(roomClientMayPush(/*pending*/ 0x1234, /*activity*/ 500, /*fails*/ 0));
  EXPECT_TRUE(roomClientMayPush(0, 500, 0));
}

// last_activity is 0 for a member that an operator added with `setperm` and who
// has not yet spoken. The node has no path to it and does not push.
TEST(RoomRetry, AClientThatNeverSpokeIsSkipped) {
  EXPECT_FALSE(roomClientMayPush(0, /*activity*/ 0, 0));
}

TEST(RoomRetry, TheAttemptsRunOutAtTheLimit) {
  for (uint8_t f = 0; f < ROOM_MAX_PUSH_FAILURES; f++) {
    EXPECT_TRUE(roomClientMayPush(0, 500, f)) << "at " << (int)f;
  }
  EXPECT_FALSE(roomClientMayPush(0, 500, ROOM_MAX_PUSH_FAILURES));
  EXPECT_FALSE(roomClientMayPush(0, 500, ROOM_MAX_PUSH_FAILURES + 1));
}

TEST(RoomRetry, NothingExpiresWhileNothingIsPending) {
  EXPECT_FALSE(roomPushAckExpired(/*pending*/ 0, /*now*/ 100000, /*timeout*/ 1));
}

TEST(RoomRetry, ThePushExpiresAtTheTimeoutAndNotBefore) {
  EXPECT_FALSE(roomPushAckExpired(0xABCD, 9999, 10000));
  EXPECT_TRUE(roomPushAckExpired(0xABCD, 10000, 10000));
  EXPECT_TRUE(roomPushAckExpired(0xABCD, 10001, 10000));
}

// The comparison is signed, so a deadline just past the wrap of millis() is
// still in the future. An unsigned compare would fire it about 49 days early.
TEST(RoomRetry, TheTimeoutSurvivesTheMillisWrap) {
  const unsigned long near_wrap = 0xFFFFF000UL;
  const unsigned long past_wrap = 0x00001000UL;   // 8 s later, after the wrap
  EXPECT_FALSE(roomPushAckExpired(0xABCD, near_wrap, past_wrap));
  EXPECT_TRUE(roomPushAckExpired(0xABCD, past_wrap + 1, past_wrap));
}

// A retry offers the SAME post again, because an unacknowledged push does not
// move the sync point of the client.
TEST(RoomRetry, ARetryOffersTheSamePostAgain) {
  PostRing ring;
  ring.add(ALICE.b, "one", 1000);
  ring.add(ALICE.b, "two", 1001);

  uint32_t sync_since = 0;
  int first = ring.nextForClient(BOB.b, sync_since, ripe(1001));
  ASSERT_GE(first, 0);
  EXPECT_EQ(ring.posts[first].post_timestamp, 1000u);

  // The push went out and got no ACK. The sync point did not move.
  uint32_t pending = 0xDEAD;
  uint8_t failures = 0;
  ASSERT_TRUE(roomPushAckExpired(pending, 20000, 10000));
  failures++;
  pending = 0;

  ASSERT_TRUE(roomClientMayPush(pending, 500, failures));
  int again = ring.nextForClient(BOB.b, sync_since, ripe(1001));
  EXPECT_EQ(again, first);
}

// Three attempts, then the client is frozen. It thaws when it talks, which is
// what resets the failure count in onClientText and onClientRequest.
TEST(RoomRetry, ThreeSilentPushesFreezeTheClientAndActivityThawsIt) {
  uint8_t failures = 0;
  for (int attempt = 0; attempt < ROOM_MAX_PUSH_FAILURES; attempt++) {
    ASSERT_TRUE(roomClientMayPush(0, 500, failures)) << "attempt " << attempt;
    ASSERT_TRUE(roomPushAckExpired(0xDEAD, 20000, 10000));
    failures++;
  }
  EXPECT_FALSE(roomClientMayPush(0, 500, failures));

  failures = 0;   // the client sent a post, a request or a keep-alive
  EXPECT_TRUE(roomClientMayPush(0, 500, failures));
}

// An ACK moves the sync point to the post that we pushed, so the scan then
// finds the post after it.
TEST(RoomRetry, AnAckMovesTheClientOnToTheNextPost) {
  PostRing ring;
  ring.add(ALICE.b, "one", 1000);
  ring.add(ALICE.b, "two", 1001);

  uint32_t sync_since = 0;
  int idx = ring.nextForClient(BOB.b, sync_since, ripe(1001));
  ASSERT_GE(idx, 0);
  uint32_t pushed = ring.posts[idx].post_timestamp;

  sync_since = pushed;   // what processRoomAck does

  int next = ring.nextForClient(BOB.b, sync_since, ripe(1001));
  ASSERT_GE(next, 0);
  EXPECT_EQ(ring.posts[next].post_timestamp, 1001u);
}

TEST(RoomRetry, ThePushCounterCountsRetriesAndNotPosts) {
  PostRing ring;
  ring.add(ALICE.b, "one", 1000);
  EXPECT_EQ(ring.num_posted, 1);
  EXPECT_EQ(ring.num_post_pushes, 0);
  ring.num_post_pushes += 3;   // one push and two retries
  EXPECT_EQ(ring.num_posted, 1);
  EXPECT_EQ(ring.num_post_pushes, 3);
}
