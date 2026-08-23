// These tests cover where a keypair goes on the filesystem, and what happens to
// a keypair that an older build left in the wrong place.
//
// The bug that they guard against: hydra passed "" to IdentityStore on every
// platform, where upstream passes "/identity" on ESP32 and RP2040. So on an M5,
// hydra wrote and read "/_main.id" while the stock firmware and upstream's own
// saveIdentity() used "/identity/_main.id". `set prv.key` reported success and
// hydra never read the result, and hydra ignored the identity of the firmware
// it replaced.
//
// The choice of directory is pure logic and needs no filesystem, so both
// platform branches run here from one binary.

#include <gtest/gtest.h>
#include <helpers/IdentityPath.h>
#include <string>

// -------------------------------------------------------- the directory itself

TEST(IdentityDir, MatchesUpstreamForEachPlatform) {
  // examples/*/main.cpp and MyMesh::saveIdentity(), word for word.
  EXPECT_STREQ("", identityDirFor(IDENTITY_DIR_FLAT));            // nRF52, STM32
  EXPECT_STREQ("/identity", identityDirFor(IDENTITY_DIR_NAMESPACED));  // ESP32, RP2040
}

TEST(IdentityDir, TheLegacyHydraDirectoryIsTheFlatOne) {
  EXPECT_STREQ("", identityLegacyDir());
}

TEST(IdentityDir, TheFlatPlatformsAreAlreadyCorrect) {
  // This is why the fix cannot change an nRF52 or STM32 build.
  EXPECT_TRUE(identityDirIsLegacy(IDENTITY_DIR_FLAT));
  EXPECT_FALSE(identityDirIsLegacy(IDENTITY_DIR_NAMESPACED));
}

TEST(IdentityDir, TheFullPathFitsTheIdentityStoreBuffer) {
  // IdentityStore builds "<dir>/<name>.id" into a char[40] with sprintf. The
  // longest hydra storage name is "_slot7" at 8 slots.
  std::string longest = std::string(identityDirFor(IDENTITY_DIR_NAMESPACED)) + "/_slot7.id";
  EXPECT_LT(longest.size() + 1, (size_t)40);
}

// ------------------------------------------------------------- the migration

TEST(IdentityMove, DoesNothingWhereTheTwoDirectoriesAreTheSame) {
  // An nRF52 board must see no filesystem activity at all, in every state.
  EXPECT_EQ(IDENTITY_MOVE_NONE, identityMoveAction(IDENTITY_DIR_FLAT, false, false));
  EXPECT_EQ(IDENTITY_MOVE_NONE, identityMoveAction(IDENTITY_DIR_FLAT, false, true));
  EXPECT_EQ(IDENTITY_MOVE_NONE, identityMoveAction(IDENTITY_DIR_FLAT, true, false));
  EXPECT_EQ(IDENTITY_MOVE_NONE, identityMoveAction(IDENTITY_DIR_FLAT, true, true));
}

TEST(IdentityMove, MovesAKeyThatOnlyTheOldPathHas) {
  EXPECT_EQ(IDENTITY_MOVE_COPY,
            identityMoveAction(IDENTITY_DIR_NAMESPACED, false, true));
}

TEST(IdentityMove, NeverOverwritesAKeyAtTheCurrentPath) {
  // Both present. The live key wins, whatever the reason the old one is there.
  EXPECT_EQ(IDENTITY_MOVE_NONE,
            identityMoveAction(IDENTITY_DIR_NAMESPACED, true, true));
}

TEST(IdentityMove, IsIdempotent) {
  // The state after a successful copy is "present at both". A second boot must
  // then do nothing.
  EXPECT_EQ(IDENTITY_MOVE_COPY,
            identityMoveAction(IDENTITY_DIR_NAMESPACED, false, true));
  EXPECT_EQ(IDENTITY_MOVE_NONE,
            identityMoveAction(IDENTITY_DIR_NAMESPACED, true, true));
}

TEST(IdentityMove, ANewBoardMintsAKeyRatherThanMigrateNothing) {
  EXPECT_EQ(IDENTITY_MOVE_NONE,
            identityMoveAction(IDENTITY_DIR_NAMESPACED, false, false));
}
