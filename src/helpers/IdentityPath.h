#pragma once

// WHERE A KEYPAIR LIVES ON THE FILESYSTEM.
//
// IdentityStore puts a file at "<dir>/<name>.id". Upstream chooses <dir> from
// the platform, and it makes that choice again in each examples/*/main.cpp and
// again in each saveIdentity():
//
//   nRF52, STM32   ""            Adafruit_LittleFS, no prefix
//   ESP32, RP2040  "/identity"
//
// The choice is a namespace convention and not a technical necessity.
// IdentityStore::begin() calls mkdir() only when the prefix starts with "/",
// and SPIFFS has a flat namespace where that mkdir does nothing at all. But a
// build that disagrees with the convention writes one file and reads a
// different one. The write reports success, and nothing ever reads it back.
// Hydra did disagree on ESP32, and the result was a node that ignored the
// identity of the firmware that it replaced.
//
// This file holds the convention ONCE. No part of it includes an Arduino, Mesh
// or target header, so a host test can drive both branches.

#include <stdint.h>
#include <string.h>

enum IdentityDirStyle : uint8_t {
  IDENTITY_DIR_FLAT       = 0,   // nRF52, STM32
  IDENTITY_DIR_NAMESPACED = 1,   // ESP32, RP2040
};

inline const char* identityDirFor(IdentityDirStyle style) {
  return style == IDENTITY_DIR_NAMESPACED ? "/identity" : "";
}

// Where hydra put its keys before it followed the convention. It is the flat
// prefix on every platform, because hydra passed "" everywhere.
inline const char* identityLegacyDir() { return ""; }

// True where the convention and the old hydra behaviour name the same file. On
// those platforms there is nothing to migrate and nothing to change.
inline bool identityDirIsLegacy(IdentityDirStyle style) {
  return strcmp(identityDirFor(style), identityLegacyDir()) == 0;
}

// ------------------------------------------------------------- the migration

enum IdentityMoveAction : uint8_t {
  IDENTITY_MOVE_NONE = 0,   // leave the filesystem as it is
  IDENTITY_MOVE_COPY = 1,   // copy the legacy file into the current directory
};

// The rules, in order:
//   1. Where the two directories are the same, there is no migration at all.
//   2. A file in the current directory wins. Never overwrite a live keypair
//      with an older one, whatever the reason it is there.
//   3. Only then does a legacy file move forward.
// A copy leaves the legacy file in place. That makes the operation repeatable
// after a reset in the middle of it, and it keeps a second copy of a keypair
// that this node cannot generate again.
inline IdentityMoveAction identityMoveAction(IdentityDirStyle style,
                                             bool at_current, bool at_legacy) {
  if (identityDirIsLegacy(style)) return IDENTITY_MOVE_NONE;
  if (at_current) return IDENTITY_MOVE_NONE;
  return at_legacy ? IDENTITY_MOVE_COPY : IDENTITY_MOVE_NONE;
}

// ------------------------------------------------- this build's own platform

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #define IDENTITY_DIR_STYLE  IDENTITY_DIR_FLAT
#elif defined(ESP32) || defined(RP2040_PLATFORM)
  #define IDENTITY_DIR_STYLE  IDENTITY_DIR_NAMESPACED
#endif

// A host build defines no platform macro, so these three stay undefined there.
// The host tests drive identityDirFor() and identityMoveAction() instead, which
// take the style as an argument and thus cover both branches from one binary.
#ifdef IDENTITY_DIR_STYLE
inline const char* identityDir() { return identityDirFor(IDENTITY_DIR_STYLE); }
inline bool identityDirIsLegacy() { return identityDirIsLegacy(IDENTITY_DIR_STYLE); }
inline IdentityMoveAction identityMoveAction(bool at_current, bool at_legacy) {
  return identityMoveAction(IDENTITY_DIR_STYLE, at_current, at_legacy);
}
#endif
