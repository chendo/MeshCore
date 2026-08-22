// Every enabled slot floods an advert (decision C), so N identities on one
// board must not transmit at the same point of the cycle. These tests cover the
// properties that the schedule must hold: slots differ, the phase stays inside
// the interval, an interval of 0 never transmits, and the phase of a slot is
// the same on every call and thus after a reboot.

#include <gtest/gtest.h>
#include <SlotPolicy.h>

namespace {
// A stand-in for the public key of a slot. Only the first 2 bytes matter.
struct Key {
  uint8_t b[32];
  Key(uint8_t hi, uint8_t lo) { memset(b, 0x5a, sizeof(b)); b[0] = hi; b[1] = lo; }
};
const uint32_t MIN60 = 60u * 60000u;
}

TEST(AdvertPhase, DifferentSlotsGetDifferentOffsets) {
  Key a(0x00, 0x00), b(0x40, 0x00), c(0xc0, 0x00);
  uint32_t oa = advertPhaseWithin(MIN60, a.b, 32);
  uint32_t ob = advertPhaseWithin(MIN60, b.b, 32);
  uint32_t oc = advertPhaseWithin(MIN60, c.b, 32);
  EXPECT_EQ(0u, oa);
  EXPECT_EQ(MIN60 / 4, ob);
  EXPECT_EQ(MIN60 / 4 * 3, oc);
  EXPECT_NE(oa, ob);
  EXPECT_NE(ob, oc);
}

TEST(AdvertPhase, TheOffsetStaysInsideTheInterval) {
  for (uint32_t mins : { 60u, 90u, 137u, 240u }) {
    uint32_t interval = mins * 60000u;
    for (int hi = 0; hi < 256; hi++) {
      Key k((uint8_t)hi, (uint8_t)(255 - hi));
      EXPECT_LT(advertPhaseWithin(interval, k.b, 32), interval) << mins << " " << hi;
    }
  }
}

TEST(AdvertPhase, AKeyThatIsTooShortGivesTheStartOfTheCycle) {
  uint8_t one = 0xff;
  EXPECT_EQ(0u, advertPhaseWithin(MIN60, &one, 1));
  EXPECT_EQ(0u, advertPhaseWithin(MIN60, nullptr, 32));
}

TEST(AdvertPhase, AnIntervalOfZeroNeverTransmits) {
  Key k(0x9e, 0x31);
  for (uint32_t now : { 0u, 1u, 123456u, 0xfffffff0u }) {
    for (uint32_t jitter : { 0u, 30000u }) {
      EXPECT_EQ(0u, nextAdvertDelay(now, 0, k.b, 32, jitter));
    }
  }
  EXPECT_EQ(0u, advertPhaseWithin(0, k.b, 32));
}

TEST(AdvertPhase, ALiveIntervalNeverReturnsZero) {
  // The caller reads 0 as "off". A slot that adverts must never produce it. The
  // bounds also say that no advert follows the one before it too closely.
  for (uint8_t mins : { 60, 90, 240 }) {
    for (int hi = 0; hi < 256; hi += 7) {
      Key k((uint8_t)hi, 0x11);
      uint32_t d = nextAdvertDelay(hi * 977u, mins, k.b, 32, 0);
      EXPECT_GT(d, 0u);
      EXPECT_GE(d, (uint32_t)mins * 60000u / 2);
      EXPECT_LE(d, (uint32_t)mins * 60000u * 3 / 2);
    }
  }
}

TEST(AdvertPhase, TheValueIsStableForAGivenSlot) {
  // The phase comes from the key, and not from a random source. So a slot keeps
  // its point of the cycle over a reboot, and two nodes that restart together
  // do not converge on one phase.
  Key k(0x3c, 0xd7);
  uint32_t first = nextAdvertDelay(5000, 60, k.b, 32, 0);
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(first, nextAdvertDelay(5000, 60, k.b, 32, 0));
  }
  EXPECT_EQ(advertPhaseWithin(MIN60, k.b, 32), advertPhaseWithin(MIN60, k.b, 32));
}

TEST(AdvertPhase, TheScheduleHoldsThePhaseAndDoesNotDrift) {
  // The delay is not "interval plus offset", which would only make the interval
  // longer on every cycle. Once the slot is on its phase, each further delay is
  // exactly one interval.
  Key k(0x80, 0x00);
  uint32_t now = 12345;
  uint32_t d = nextAdvertDelay(now, 60, k.b, 32, 0);
  for (int i = 0; i < 24; i++) {
    now += d;
    d = nextAdvertDelay(now, 60, k.b, 32, 0);
    EXPECT_EQ(MIN60, d) << "cycle " << i;
  }
}

TEST(AdvertPhase, JitterDoesNotAccumulate) {
  // A cycle that runs late by its jitter comes back to the phase of the key.
  Key k(0x22, 0x99);
  uint32_t now = 999;
  now += nextAdvertDelay(now, 60, k.b, 32, 0);   // now sits on the phase
  for (uint32_t jitter : { 30000u, 1u, 29999u, 12345u }) {
    uint32_t d = nextAdvertDelay(now, 60, k.b, 32, jitter);
    EXPECT_EQ(MIN60 + jitter, d);
    now += d;                                    // late by the jitter
    uint32_t back = nextAdvertDelay(now, 60, k.b, 32, 0);
    EXPECT_EQ(MIN60 - jitter, back);             // the next cycle takes it back
    now += back;
  }
}

TEST(AdvertPhase, ThreeSlotsOfOneNodeSpreadAcrossTheCycle) {
  // The case that this code is for. A board with three identities that all
  // flood advert on the same interval.
  Key s1(0x11, 0x00), s2(0x6a, 0x00), s3(0xd0, 0x00);
  uint32_t now = 60000;
  uint32_t t1 = now + nextAdvertDelay(now, 60, s1.b, 32, 0);
  uint32_t t2 = now + nextAdvertDelay(now, 60, s2.b, 32, 0);
  uint32_t t3 = now + nextAdvertDelay(now, 60, s3.b, 32, 0);
  // No two of them land within 5 minutes of each other.
  EXPECT_GT((t2 > t1 ? t2 - t1 : t1 - t2), 5u * 60000u);
  EXPECT_GT((t3 > t2 ? t3 - t2 : t2 - t3), 5u * 60000u);
  EXPECT_GT((t3 > t1 ? t3 - t1 : t1 - t3), 5u * 60000u);
}

TEST(AdvertPhase, TheStartAdvertOfEachSlotIsSpreadToo) {
  // Every slot starts in the same pass of setup(), so the fixed delay of the
  // first advert would put all of them on the air together.
  Key s1(0x11, 0x00), s2(0x6a, 0x00), s3(0xd0, 0x00);
  uint32_t a = advertPhaseWithin(30000, s1.b, 32);
  uint32_t b = advertPhaseWithin(30000, s2.b, 32);
  uint32_t c = advertPhaseWithin(30000, s3.b, 32);
  EXPECT_LT(a, 30000u);
  EXPECT_LT(b, 30000u);
  EXPECT_LT(c, 30000u);
  EXPECT_GT(b - a, 5000u);
  EXPECT_GT(c - b, 5000u);
}
