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
static bool multiIdLoad(const char* role, fs::FS* fs, mesh::LocalIdentity& id) {
  IdentityStore store(*fs, "/identity");
  store.begin();

  Preferences nvs;
  nvs.begin("multiids", false);

  if (store.load("_main", id)) {
    uint8_t blob[MULTI_ID_BLOB];
    if (multiIdToBlob(id, blob)) nvs.putBytes(role, blob, MULTI_ID_BLOB);
    nvs.end();
    return true;
  }

  // FS copy missing or unreadable — try the mirror before minting a new key
  uint8_t blob[MULTI_ID_BLOB];
  size_t got = nvs.getBytes(role, blob, sizeof(blob));
  nvs.end();
  if (got == MULTI_ID_BLOB) {
    id = multiIdFromBlob(blob);
    store.save("_main", id);   // heal the filesystem copy
    Serial.printf("[%s] identity RESTORED from NVS mirror (filesystem copy was missing/corrupt)\n", role);
    return true;
  }
  return false;
}

// Save a freshly-created identity into the NVS mirror.
static void multiIdImportSaveMirror(const char* role, mesh::LocalIdentity& id) {
  uint8_t blob[MULTI_ID_BLOB];
  if (!multiIdToBlob(id, blob)) return;
  Preferences nvs;
  nvs.begin("multiids", false);
  nvs.putBytes(role, blob, MULTI_ID_BLOB);
  nvs.end();
}

// Import an identity from a 192-hex-char blob (pub||prv), writing both the
// filesystem copy and the mirror. Used by the console recovery command.
static bool multiIdImport(const char* role, fs::FS* fs, const char* hex192) {
  if (hex192 == nullptr || strlen(hex192) != MULTI_ID_BLOB * 2) return false;
  uint8_t blob[MULTI_ID_BLOB];
  if (!mesh::Utils::fromHex(blob, MULTI_ID_BLOB, hex192)) return false;

  mesh::LocalIdentity id = multiIdFromBlob(blob);

  IdentityStore store(*fs, "/identity");
  store.begin();
  if (!store.save("_main", id)) return false;

  Preferences nvs;
  nvs.begin("multiids", false);
  nvs.putBytes(role, blob, MULTI_ID_BLOB);
  nvs.end();
  return true;
}
