// These tests cover the parts of the hydra slot CLI that hold real logic: the
// rule that a name comes before enable, the RAM reserve floor, how safe mode
// decodes a stored slot record, and the split between the node namespace and
// the slot namespace.

#include <gtest/gtest.h>
#include <SlotPolicy.h>

// ---------------------------------------------------------------- slot naming

TEST(SlotName, EmptyIsNotAName) {
  EXPECT_FALSE(slotNameValid(""));
  EXPECT_FALSE(slotNameValid(nullptr));
}

TEST(SlotName, OrdinaryNamesPass) {
  EXPECT_TRUE(slotNameValid("Hilltop Room"));
  EXPECT_TRUE(slotNameValid("a"));
}

TEST(SlotName, RejectsTheCharactersThatBreakAdvertListings) {
  EXPECT_FALSE(slotNameValid("bad[name"));
  EXPECT_FALSE(slotNameValid("bad]name"));
  EXPECT_FALSE(slotNameValid("bad\\name"));
  EXPECT_FALSE(slotNameValid("bad:name"));
  EXPECT_FALSE(slotNameValid("bad,name"));
  EXPECT_FALSE(slotNameValid("bad?name"));
  EXPECT_FALSE(slotNameValid("bad*name"));
}

TEST(SlotName, RejectsAnythingThatWouldNotFitTheField) {
  std::string longest(SLOT_NAME_MAX - 1, 'x');
  EXPECT_TRUE(slotNameValid(longest.c_str()));
  EXPECT_FALSE(slotNameValid((longest + "x").c_str()));
}

// ---------------------------------------------------- the name-before-enable rule

TEST(SlotEnable, RefusesAnUnnamedSlot) {
  EXPECT_EQ(SLOT_ENABLE_NO_NAME, slotEnableCheck(1, 3, SLOT_CHAT, ""));
  EXPECT_EQ(SLOT_ENABLE_NO_NAME, slotEnableCheck(1, 3, SLOT_ROOM, ""));
}

TEST(SlotEnable, RefusesANameItWouldNotHaveAccepted) {
  // This check uses the same test as `set name`. An older build could accept a
  // bad name. Such a record still cannot start an identity.
  EXPECT_EQ(SLOT_ENABLE_NO_NAME, slotEnableCheck(1, 3, SLOT_CHAT, "no:colons"));
}

TEST(SlotEnable, AcceptsANamedChatOrRoomSlot) {
  EXPECT_EQ(SLOT_ENABLE_OK, slotEnableCheck(1, 3, SLOT_CHAT, "Sunset"));
  EXPECT_EQ(SLOT_ENABLE_OK, slotEnableCheck(2, 3, SLOT_ROOM, "Sunset"));
}

TEST(SlotEnable, Slot0IsNeverToggled) {
  EXPECT_EQ(SLOT_ENABLE_SLOT0, slotEnableCheck(0, 3, SLOT_CHAT, "Sunset"));
}

TEST(SlotEnable, RangeIsChecked) {
  EXPECT_EQ(SLOT_ENABLE_RANGE, slotEnableCheck(3, 3, SLOT_CHAT, "Sunset"));
  EXPECT_EQ(SLOT_ENABLE_RANGE, slotEnableCheck(-1, 3, SLOT_CHAT, "Sunset"));
}

TEST(SlotEnable, OffAndRepeaterAreNotThingsASlotCanBeTurnedInto) {
  EXPECT_EQ(SLOT_ENABLE_BAD_TYPE, slotEnableCheck(1, 3, SLOT_OFF, "Sunset"));
  EXPECT_EQ(SLOT_ENABLE_BAD_TYPE, slotEnableCheck(1, 3, SLOT_REPEATER, "Sunset"));
}

TEST(SlotEnable, EveryRefusalSaysWhy) {
  // The rule exists to tell the operator the cause. Thus no result may give only
  // "ERR".
  const SlotEnableResult all[] = {
    SLOT_ENABLE_RANGE, SLOT_ENABLE_SLOT0, SLOT_ENABLE_NO_NAME,
    SLOT_ENABLE_BAD_TYPE, SLOT_ENABLE_NO_RAM, SLOT_ENABLE_FAILED,
  };
  for (SlotEnableResult r : all) {
    EXPECT_GT(strlen(slotEnableError(r)), 8u) << "result " << (int)r;
    EXPECT_STRNE("ERR", slotEnableError(r));
  }
}

// ------------------------------------------------- safe-mode record decoding

TEST(SlotRecord, AnUnknownTypeByteBecomesOffRatherThanSomethingElse) {
  EXPECT_EQ(SLOT_OFF, slotTypeFromByte(0));
  EXPECT_EQ(SLOT_CHAT, slotTypeFromByte(SLOT_CHAT));
  EXPECT_EQ(SLOT_ROOM, slotTypeFromByte(SLOT_ROOM));
  EXPECT_EQ(SLOT_OFF, slotTypeFromByte(SLOT_REPEATER));   // slot 0 only
  for (int b = 4; b < 256; b++) EXPECT_EQ(SLOT_OFF, slotTypeFromByte((uint8_t)b));
}

TEST(SlotRecord, ANamelessStoredSlotComesUpOff) {
  EXPECT_EQ(SLOT_OFF, slotTypeFromRecord(SLOT_CHAT, ""));
  EXPECT_EQ(SLOT_OFF, slotTypeFromRecord(SLOT_ROOM, ""));
  EXPECT_EQ(SLOT_CHAT, slotTypeFromRecord(SLOT_CHAT, "Sunset"));
}

TEST(SlotRecord, GarbageDecodesToOffNotToARunningIdentity) {
  // A short read or an erased sector gives a record of 0xFF bytes.
  char junk[SLOT_NAME_MAX];
  memset(junk, 0xFF, sizeof(junk));
  junk[SLOT_NAME_MAX - 1] = 0;
  EXPECT_EQ(SLOT_OFF, slotTypeFromRecord(0xFF, junk));
}

// ------------------------------------------------------------ the RAM reserve

namespace {
// This heap has a fixed budget and a limit on the largest block. Thus the tests
// can use the refusal path. A real heap that is nearly full is not necessary.
struct FakeHeap {
  static size_t budget;
  static size_t max_block;
  static int    live;
  static void* alloc(size_t n) {
    if (n > max_block || n > budget) return nullptr;
    budget -= n;
    live++;
    size_t* p = (size_t*)malloc(n + sizeof(size_t));
    if (!p) return nullptr;
    *p = n;
    return p + 1;
  }
  static void release(void* p) {
    if (!p) return;
    size_t* base = ((size_t*)p) - 1;
    budget += *base;
    live--;
    free(base);
  }
  static void reset(size_t b, size_t m) { budget = b; max_block = m; live = 0; }
};
size_t FakeHeap::budget = 0;
size_t FakeHeap::max_block = 0;
int    FakeHeap::live = 0;
}

TEST(RamFloor, PinsTheReserveWhenThereIsRoom) {
  FakeHeap::reset(64 * 1024, 64 * 1024);
  RamFloor floor(HYDRA_RAM_RESERVE, FakeHeap::alloc, FakeHeap::release);
  EXPECT_TRUE(floor.held());
  EXPECT_EQ(1, FakeHeap::live);
}

TEST(RamFloor, RefusesWhenTheHeapIsAlreadyBelowTheFloor) {
  FakeHeap::reset(HYDRA_RAM_RESERVE - 1, 64 * 1024);
  RamFloor floor(HYDRA_RAM_RESERVE, FakeHeap::alloc, FakeHeap::release);
  EXPECT_FALSE(floor.held());
}

TEST(RamFloor, ASlotThatWouldEatIntoTheReserveCannotAllocate) {
  // This is the full rule. The code holds the reserve FIRST. Thus the slot takes
  // its own memory from what remains, not from the whole heap.
  const size_t slot_need = 8 * 1024;
  FakeHeap::reset(HYDRA_RAM_RESERVE + slot_need / 2, 64 * 1024);
  RamFloor floor(HYDRA_RAM_RESERVE, FakeHeap::alloc, FakeHeap::release);
  ASSERT_TRUE(floor.held());
  EXPECT_EQ(nullptr, FakeHeap::alloc(slot_need));   // the heap must refuse: that is the point
}

TEST(RamFloor, ASlotThatFitsAboveTheReserveStillAllocates) {
  const size_t slot_need = 8 * 1024;
  FakeHeap::reset(HYDRA_RAM_RESERVE + slot_need, 64 * 1024);
  RamFloor floor(HYDRA_RAM_RESERVE, FakeHeap::alloc, FakeHeap::release);
  ASSERT_TRUE(floor.held());
  void* p = FakeHeap::alloc(slot_need);
  EXPECT_NE(nullptr, p);
  FakeHeap::release(p);
}

TEST(RamFloor, ReleasingHandsTheReserveBack) {
  FakeHeap::reset(64 * 1024, 64 * 1024);
  {
    RamFloor floor(HYDRA_RAM_RESERVE, FakeHeap::alloc, FakeHeap::release);
    ASSERT_TRUE(floor.held());
    floor.release();
    EXPECT_EQ(0, FakeHeap::live);
    floor.release();   // a second release does nothing: the destructor must not free it twice
  }
  EXPECT_EQ(0, FakeHeap::live);
  EXPECT_EQ(64u * 1024u, FakeHeap::budget);
}

TEST(RamFloor, DestructorHandsTheReserveBack) {
  FakeHeap::reset(64 * 1024, 64 * 1024);
  { RamFloor floor(HYDRA_RAM_RESERVE, FakeHeap::alloc, FakeHeap::release); }
  EXPECT_EQ(0, FakeHeap::live);
}

TEST(RamFloor, AZeroReserveNeverRefuses) {
  FakeHeap::reset(0, 0);
  RamFloor floor(0, FakeHeap::alloc, FakeHeap::release);
  EXPECT_TRUE(floor.held());
  EXPECT_EQ(0, FakeHeap::live);
}

TEST(HeadroomProbe, ReportsTheLargestBlockTheHeapWouldHandOut) {
  FakeHeap::reset(64 * 1024, 5000);
  EXPECT_EQ(5000u, probeLargestBlock(64 * 1024, FakeHeap::alloc, FakeHeap::release));
  EXPECT_EQ(0, FakeHeap::live) << "the probe must not keep anything";
}

TEST(HeadroomProbe, IsCappedByItsCeiling) {
  FakeHeap::reset(1024 * 1024, 1024 * 1024);
  EXPECT_EQ(4096u, probeLargestBlock(4096, FakeHeap::alloc, FakeHeap::release));
}

TEST(HeadroomProbe, ReportsZeroOnAnExhaustedHeap) {
  FakeHeap::reset(0, 0);
  EXPECT_EQ(0u, probeLargestBlock(64 * 1024, FakeHeap::alloc, FakeHeap::release));
}

// -------------------------------------------------- node vs slot namespace

TEST(CliNamespace, SlotLevelRefusesRadioParameters) {
  // Decision 8: the board has one transceiver, and slot 3 must not tune it.
  for (const char* v : { "freq", "bw", "sf", "cr", "tx", "radio", "cad",
                         "int.thresh", "dutycycle", "af", "extra.sf" }) {
    EXPECT_TRUE(slotVerbIsNodeLevel(v)) << v;
  }
}

TEST(CliNamespace, DottedChildrenOfANodeKeyAreAlsoNodeLevel) {
  EXPECT_TRUE(slotVerbIsNodeLevel("radio.rxgain on"));
  EXPECT_TRUE(slotVerbIsNodeLevel("radio.fem.txgain on"));
  EXPECT_TRUE(slotVerbIsNodeLevel("bridge.baud 115200"));
  EXPECT_TRUE(slotVerbIsNodeLevel("agc.reset.interval 4"));
}

TEST(CliNamespace, SlotLevelRefusesBoardAndNodeWideVerbs) {
  for (const char* v : { "reboot", "erase", "time", "clock", "log start",
                         "password x", "ver", "neighbors", "powersave on" }) {
    EXPECT_TRUE(slotVerbIsNodeLevel(v)) << v;
  }
}

TEST(CliNamespace, SlotLevelOwnsIdentityNameAclAndAdvertSettings) {
  for (const char* v : { "name Sunset", "prv.key aabb", "advert.interval 30",
                         "flood.advert on", "setperm aabb 2", "advert",
                         "flood advert", "contacts", "acl", "posts", "type",
                         "public.key" }) {
    EXPECT_FALSE(slotVerbIsNodeLevel(v)) << v;
  }
}

TEST(CliNamespace, MatchesWholeTokensNotPrefixes) {
  // "tx" is at node level. Names that are almost the same, such as "txt" and
  // "tx.foo", are not at node level.
  EXPECT_TRUE(slotVerbIsNodeLevel("tx 20"));
  EXPECT_FALSE(slotVerbIsNodeLevel("txname"));
  EXPECT_FALSE(slotVerbIsNodeLevel("radioactive"));
  EXPECT_FALSE(slotVerbIsNodeLevel("names"));
  EXPECT_FALSE(slotVerbIsNodeLevel(""));
  EXPECT_FALSE(slotVerbIsNodeLevel(nullptr));
}
