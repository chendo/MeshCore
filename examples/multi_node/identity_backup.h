#pragma once

// Identity durability for the composition.
//
// The stock examples all do:
//     if (!store.load("_main", self_id)) { self_id = new_identity(); store.save(...); }
// which means ONE failed read — a corrupted file, a transient FS error, a
// full filesystem — permanently destroys the node's address: a brand new
// identity is generated and written straight over the old one. Every peer
// that knew this node now sees a stranger, and the old key is unrecoverable.
// This happened on the M5 (the repeater's key changed under us).
//
// So: mirror every identity into NVS (a different storage technology on a
// different flash region) the first time it loads, and on a failed FS load
// restore from that mirror instead of minting a new one. Only when BOTH are
// unavailable is a new identity created.

#include <Arduino.h>
#include <Preferences.h>
#include <helpers/IdentityStore.h>

// Canonical 96-byte blob used here: pub_key[32] then prv_key[64] — the same
// order IdentityStore writes to file (NOTE: LocalIdentity::writeTo(uint8_t*)
// emits the REVERSE, prv then pub, so conversions below are explicit).
static const size_t MULTI_ID_BLOB = PUB_KEY_SIZE + PRV_KEY_SIZE;

static bool multiIdToBlob(mesh::LocalIdentity& id, uint8_t blob[MULTI_ID_BLOB]) {
  uint8_t tmp[PRV_KEY_SIZE + PUB_KEY_SIZE];
  if (id.writeTo(tmp, sizeof(tmp)) != sizeof(tmp)) return false;   // prv || pub
  memcpy(blob, tmp + PRV_KEY_SIZE, PUB_KEY_SIZE);                  // -> pub
  memcpy(blob + PUB_KEY_SIZE, tmp, PRV_KEY_SIZE);                  // -> prv
  return true;
}

static mesh::LocalIdentity multiIdFromBlob(const uint8_t blob[MULTI_ID_BLOB]) {
  char pub_hex[PUB_KEY_SIZE * 2 + 1], prv_hex[PRV_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(pub_hex, blob, PUB_KEY_SIZE);
  mesh::Utils::toHex(prv_hex, blob + PUB_KEY_SIZE, PRV_KEY_SIZE);
  return mesh::LocalIdentity(prv_hex, pub_hex);
}

// Load `role`'s identity, preferring the filesystem, falling back to the NVS
// mirror. Returns false only if neither source has one (caller then creates a
// new identity). On success the mirror is refreshed.
// Records how each identity was obtained this boot (see 'identities source').
// Storage lives in ONE translation unit (main.cpp): this header is included by
// every wrapper, so a `static` array here would give each wrapper its own
// private copy and the composition would always read an empty log.
void multiIdNoteSource(const char* role, const char* how);
int  multiIdSourceReport(char* out, size_t cap);

static bool multiIdLoad(const char* role, fs::FS* fs, mesh::LocalIdentity& id) {
  IdentityStore store(*fs, "/identity");
  store.begin();

  Preferences nvs;
  nvs.begin("multiids", false);

  uint8_t mirror[MULTI_ID_BLOB];
  bool have_mirror = nvs.getBytes(role, mirror, sizeof(mirror)) == MULTI_ID_BLOB;

  if (store.load("_main", id)) {
    uint8_t blob[MULTI_ID_BLOB];
    bool ok = multiIdToBlob(id, blob);
    // THE FILESYSTEM COPY IS AUTHORITATIVE. An earlier version treated a
    // disagreement with the mirror as corruption and forced the mirror back —
    // which silently reverted deliberate key changes (a rekey applied by any
    // path that writes only the file) and was extremely hard to see. The
    // mirror exists purely as a fallback for when the file is GONE, never as
    // a veto over what the operator set.
    if (ok && (!have_mirror || memcmp(blob, mirror, MULTI_ID_BLOB) != 0)) {
      nvs.putBytes(role, blob, MULTI_ID_BLOB);   // follow the file
    }
    nvs.end();
    multiIdNoteSource(role, "filesystem");
    return true;
  }

  // FS copy missing or unreadable — try the mirror before minting a new key
  nvs.end();
  if (have_mirror) {
    id = multiIdFromBlob(mirror);
    store.save("_main", id);   // heal the filesystem copy
    Serial.printf("[%s] identity RESTORED from NVS mirror (filesystem copy was missing/corrupt)\n", role);
    multiIdNoteSource(role, "mirror (fs copy unreadable)");
    return true;
  }
  multiIdNoteSource(role, "NEW KEY CREATED");
  return false;
}

// Save an identity into the NVS mirror, but NEVER over an existing entry.
//
// This is the safety catch that was missing: when a load failed for a
// transient reason (NVS pressure, a busy filesystem) the caller minted a new
// identity and called this, overwriting the good backup with the freshly
// invented key — making the loss permanent. A mirror is only ever written here
// when there is nothing to lose. Deliberate rekeys go through multiIdImport(),
// which is allowed to overwrite.
// Deliberate rekey / restore: this MUST replace whatever is stored, otherwise
// a bad mirror would keep winning the mismatch check and undo the operator's
// change on the next boot.
static void multiIdSaveMirrorForce(const char* role, mesh::LocalIdentity& id) {
  uint8_t blob[MULTI_ID_BLOB];
  if (!multiIdToBlob(id, blob)) return;
  Preferences nvs;
  if (!nvs.begin("multiids", false)) {
    Serial.printf("[%s] WARNING: identity mirror unavailable (NVS full?)\n", role);
    return;
  }
  nvs.putBytes(role, blob, MULTI_ID_BLOB);
  nvs.end();
}

static void multiIdImportSaveMirror(const char* role, mesh::LocalIdentity& id) {
  uint8_t blob[MULTI_ID_BLOB];
  if (!multiIdToBlob(id, blob)) return;
  Preferences nvs;
  if (!nvs.begin("multiids", false)) {
    Serial.printf("[%s] WARNING: identity mirror unavailable (NVS full?)\n", role);
    return;
  }
  uint8_t existing[MULTI_ID_BLOB];
  bool have = nvs.getBytes(role, existing, sizeof(existing)) == MULTI_ID_BLOB;
  if (have && memcmp(existing, blob, MULTI_ID_BLOB) != 0) {
    nvs.end();
    Serial.printf("[%s] REFUSING to overwrite the saved identity with a different key\n", role);
    return;
  }
  if (!have) nvs.putBytes(role, blob, MULTI_ID_BLOB);
  nvs.end();
}

// Import an identity from a 192-hex-char blob (pub||prv), writing both the
// filesystem copy and the mirror. Used by the console recovery command.
// Accepts EITHER a 128-hex private key (the format the stock 'set prv.key'
// and the phone app use — the public key is derived from it) or the 192-hex
// pub||prv blob written by the mirror.
// Returns nullptr on success, otherwise WHY it failed. A single boolean was
// hiding which stage broke: a rejected key, a hex parse failure and a
// filesystem write failure all surfaced as "bad private key", which sent us
// looking at a key that was provably fine.
static const char* multiIdImportReason(const char* role, fs::FS* fs, const char* hex) {
  if (hex == nullptr) return "no key supplied";
  size_t n = strlen(hex);
  mesh::LocalIdentity id;

  if (n == PRV_KEY_SIZE * 2) {                 // 128 hex: private key only
    uint8_t prv[PRV_KEY_SIZE];
    if (!mesh::Utils::fromHex(prv, PRV_KEY_SIZE, hex)) return "not valid hex";
    if (!mesh::LocalIdentity::validatePrivateKey(prv)) return "key rejected by ed25519 validation";
    id.readFrom(prv, PRV_KEY_SIZE);            // derives the public key
  } else if (n == MULTI_ID_BLOB * 2) {         // 192 hex: pub||prv blob
    uint8_t blob[MULTI_ID_BLOB];
    if (!mesh::Utils::fromHex(blob, MULTI_ID_BLOB, hex)) return "not valid hex";
    if (!mesh::LocalIdentity::validatePrivateKey(blob + PUB_KEY_SIZE)) return "key rejected by ed25519 validation";
    id = multiIdFromBlob(blob);
  } else {
    return "wrong length";
  }

  if (fs == nullptr) return "no filesystem for that role";
  IdentityStore store(*fs, "/identity");
  store.begin();
  if (!store.save("_main", id)) return "filesystem write failed";

  multiIdSaveMirrorForce(role, id);   // deliberate change: the mirror must follow
  return nullptr;
}

static bool multiIdImport(const char* role, fs::FS* fs, const char* hex) {
  return multiIdImportReason(role, fs, hex) == nullptr;
}
