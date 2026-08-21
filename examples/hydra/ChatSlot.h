#pragma once

// Slots 1..N: an identity that is not the repeater — a chat client or a room
// server. One class covers both because everything below the ACL and the post
// buffer is identical: a keypair, a name, contacts, adverts, and no forwarding.
//
// SCAFFOLDING. This boots, gets its own keypair, adverts, and tracks contacts —
// enough to prove a second identity can live on the shared radio and be seen by
// the mesh as a separate node. It has no companion interface and answers no
// messages; the message/ack callbacks are deliberately empty.
//
// SLOT_ROOM IS PARTIAL. A room slot gets a real, private ClientACL and a real
// post buffer, and adverts as ADV_TYPE_ROOM, so membership and the RAM
// accounting behave as they will. The room PROTOCOL — post delivery, client
// sync, push retries — is not implemented; onCommandDataRecv is still empty.

#include "HydraSlot.h"
#include "RoomStore.h"
#include "SlotMeshTables.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <target.h>   // board, radio_driver, rtc_clock: node-scoped, one each

// Non-repeater slots are sized down hard: sizeof(mesh::Packet) is 262 B, so the
// repeater's 32-entry pool is ~8.3 KB on its own. A chat identity only ever has
// its own traffic in flight — it forwards nothing — so 8 is generous.
#ifndef HYDRA_CHAT_POOL
  #define HYDRA_CHAT_POOL     8
#endif
#ifndef HYDRA_CHAT_HASHES
  #define HYDRA_CHAT_HASHES  32
#endif

#ifndef HYDRA_CHAT_ADVERT_MINS
  #define HYDRA_CHAT_ADVERT_MINS  60
#endif

class ChatMesh : public BaseChatMesh {
  char _name[SLOT_NAME_MAX];
  unsigned long _next_advert;
  uint8_t _advert_mins;
  bool _flood;
  uint8_t _adv_type;

protected:
  float getAirtimeBudgetFactor() const override { return 1.0f; }
  int calcRxDelay(float score, uint32_t air_time) const override { return 0; }

  // A chat identity is an endpoint, not a relay. Slot 0 is already forwarding
  // everything this radio hears; a second identity forwarding the same floods
  // would put two copies of each on the air from one antenna.
  bool allowPacketForward(const mesh::Packet* packet) override { return false; }

  void onDiscoveredContact(ContactInfo& c, bool is_new, uint8_t path_len, const uint8_t* path) override {}
  void onContactPathUpdated(const ContactInfo& c) override {}
  ContactInfo* processAck(const uint8_t* data) override { return NULL; }
  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {}
  void onCommandDataRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override {}
  void onSignedMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const uint8_t* prefix, const char* text) override {}
  void onChannelMessageRecv(const mesh::GroupChannel& ch, mesh::Packet* pkt, uint32_t ts, const char* text) override {}
  uint8_t onContactRequest(const ContactInfo& c, uint32_t ts, const uint8_t* data, uint8_t len, uint8_t* reply) override { return 0; }
  void onContactResponse(const ContactInfo& c, const uint8_t* data, uint8_t len) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t air_ms) const override { return 12000 + 16 * air_ms; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t air_ms, uint8_t path_len) const override {
    return 12000 + (air_ms * 4 + 500) * ((path_len & 63) + 1);
  }
  void onSendTimeout() override {}

public:
  ChatMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
           mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
      // Flood by default: a slot nobody can discover is not useful, and an
      // operator should not have to know to turn discovery on. Airtime is
      // bounded by _advert_mins, which is the dial that matters.
      : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _next_advert(0),
        _advert_mins(HYDRA_CHAT_ADVERT_MINS), _flood(true), _adv_type(ADV_TYPE_CHAT) {
    _name[0] = 0;
  }

  const char* nodeName() const { return _name; }
  void setNodeName(const char* n) { StrHelper::strncpy(_name, n, sizeof(_name)); }
  void setAdvertType(uint8_t t) { _adv_type = t; }
  bool hasPendingWork() const { return _mgr->getOutboundTotal() > 0; }

  uint8_t advertMins() const { return _advert_mins; }
  void setAdvertMins(uint8_t m) { _advert_mins = m; }
  bool floodAdvert() const { return _flood; }
  void setFloodAdvert(bool f) { _flood = f; }

  void advertise(uint32_t delay_millis, bool flood) {
    uint8_t app_data[MAX_ADVERT_DATA_SIZE];
    uint8_t len;
    {
      AdvertDataBuilder builder(_adv_type, _name);
      len = builder.encodeTo(app_data);
    }
    mesh::Packet* pkt = createAdvert(self_id, app_data, len);
    if (pkt == NULL) return;
    if (flood) sendFlood(pkt, delay_millis); else sendZeroHop(pkt, delay_millis);
    // Decision C: the interval starts when the slot is enabled, so enabling is
    // also the announce-myself moment rather than a wait for the first cycle.
    _next_advert = _advert_mins ? futureMillis((int)(_advert_mins * 60000UL)) : 0;
  }

  void loop() {
    BaseChatMesh::loop();
    // zero until the slot's first advert, so a slot that never started is quiet
    if (_advert_mins > 0 && _next_advert != 0 && millisHasNowPassed(_next_advert)) {
      advertise(0, _flood);
    }
  }
};

class ChatSlot : public HydraSlot {
  RadioPort                       _port;
  ArduinoMillis                   _ms;
  StdRNG                          _rng;
  SlotMeshTables<HYDRA_CHAT_HASHES> _tables;
  DeferredPacketManager           _mgr;
  ChatMesh                        _mesh;
  RoomStore*                      _room;   // non-null only while a room slot runs
  FILESYSTEM*                     _fs;
  char                            _acl_file[20];
  unsigned long                   _acl_dirty_at;   // 0 = clean
  SlotType                        _type;
  bool                            _begun;

public:
  ChatSlot()
      : _mgr(HYDRA_CHAT_POOL), _mesh(_port, _ms, _rng, rtc_clock, _mgr, _tables),
        _room(nullptr), _fs(nullptr), _acl_dirty_at(0), _type(SLOT_OFF), _begun(false) {
    _acl_file[0] = 0;
  }

  RadioPort& port() override { return _port; }
  SlotType type() const override { return _type; }
  const mesh::LocalIdentity& identity() const override { return _mesh.self_id; }
  const char* name() const override { return _mesh.nodeName(); }
  void setName(const char* n) override { _mesh.setNodeName(n); }
  bool hasPendingWork() const override { return _begun && _mesh.hasPendingWork(); }
  ChatMesh& mesh() { return _mesh; }   // hook for the companion facade / diag bot
  RoomStore* room() { return _room; }

  // What enabling this slot will ask the heap for, before it asks. Reporting
  // only — the actual gate is RamFloor, which pins the reserve and then lets
  // these allocations run against what is left.
  static size_t heapCost(SlotType t) {
    size_t n = (size_t)HYDRA_CHAT_POOL * sizeof(mesh::Packet) + 256;
    if (t == SLOT_ROOM) n += RoomStore::heapCost();
    return n;
  }

  bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name,
             const char* display_name, SlotType type) override {
    if (_begun) return true;
    if (!_mgr.allocatePool()) return false;   // heap refused: RAM floor did its job
    _fs = fs;
    if (!store.load(id_name, _mesh.self_id)) {
      // radio_new_identity() samples RSSI noise, which drops the transceiver
      // out of receive. Only on first boot of a slot, and the next pump()
      // re-arms RX, so it costs one loop iteration of deafness.
      _mesh.self_id = radio_new_identity();
      for (int i = 0; i < 10 && (_mesh.self_id.pub_key[0] == 0x00 || _mesh.self_id.pub_key[0] == 0xFF); i++) {
        _mesh.self_id = radio_new_identity();
      }
      store.save(id_name, _mesh.self_id);
    }
    _mesh.setNodeName(display_name);
    _mesh.setAdvertType(type == SLOT_ROOM ? ADV_TYPE_ROOM : ADV_TYPE_CHAT);

    if (type == SLOT_ROOM) {
      _room = new (std::nothrow) RoomStore();
      if (_room == nullptr) return false;   // as above: refuse, do not half-start
      // Scoped by the slot's STORAGE name, which is fixed at creation and does
      // not follow the type (decision 7), so retyping a slot keeps its members.
      snprintf(_acl_file, sizeof(_acl_file), "/acl%s", id_name);
      _room->load(_fs, _mesh.self_id, _acl_file);
    }

    // Only the base begin(). Unlike the repeater's MyMesh::begin(), a chat slot
    // pushes no freq/bw/sf/cr or TX power at the radio — those are node
    // settings, owned by slot 0's prefs, and there is one transceiver.
    _mesh.begin();
    _type = type;
    _begun = true;
    _mesh.advertise(8000, _mesh.floodAdvert());
    return true;
  }

  // Disabling a slot silences its port but does not free the room: bringing it
  // back must not depend on the heap still having a contiguous block months
  // later, and re-running _mesh.begin() is not something this scaffolding
  // promises. The ACL is flushed here so nothing pending is lost.
  void flushPendingWrites() override {
    if (_room && _acl_dirty_at && _fs) {
      _room->save(_fs, _acl_file);
      _acl_dirty_at = 0;
    }
  }

  void loop() override {
    if (!_begun) return;
    _mesh.loop();
    // Coalesced the same way the repeater coalesces its ACL, and for the same
    // reason: the write blocks the loop for ~1.6 s and wears the flash, so a
    // run of `setperm`s costs one write rather than one each.
    if (_acl_dirty_at && (long)(millis() - _acl_dirty_at) >= 0) flushPendingWrites();
  }

  void handleCommand(uint32_t sender_timestamp, char* command,
                     char* reply, size_t reply_sz) override {
    if (!_begun) { StrHelper::strncpy(reply, "slot not running", reply_sz); return; }

    if (strcmp(command, "advert") == 0) {
      _mesh.advertise(0, false);
      StrHelper::strncpy(reply, "OK - zero-hop advert queued", reply_sz);
    } else if (strcmp(command, "flood advert") == 0) {
      _mesh.advertise(0, true);
      StrHelper::strncpy(reply, "OK - flood advert queued", reply_sz);
    } else if (strcmp(command, "contacts") == 0) {
      snprintf(reply, reply_sz, "contacts: %d", _mesh.getNumContacts());
    } else if (strncmp(command, "setperm ", 8) == 0) {
      handleSetPerm(command + 8, reply, reply_sz);
    } else if (strcmp(command, "clear acl") == 0) {
      if (_room == nullptr) { StrHelper::strncpy(reply, "ERR: not a room slot", reply_sz); return; }
      // Demoting to guest is how ClientACL deletes. Bail if the count ever
      // fails to drop rather than spinning the loop watchdog into a reboot.
      for (int n = _room->acl.getNumClients(); n > 0; n = _room->acl.getNumClients()) {
        ClientInfo* c = _room->acl.getClientByIdx(0);
        _room->acl.applyPermissions(_mesh.self_id, c->id.pub_key, PUB_KEY_SIZE, PERM_ACL_GUEST);
        if (_room->acl.getNumClients() >= n) break;
      }
      markAclDirty();
      StrHelper::strncpy(reply, "OK - room ACL cleared", reply_sz);
    } else if (sender_timestamp == 0 && strcmp(command, "acl") == 0) {
      // Reached as `slot N get acl` (HydraNode strips the "get "). Serial only,
      // like the repeater's `get acl`: it lists third parties.
      if (_room == nullptr) { StrHelper::strncpy(reply, "ERR: not a room slot", reply_sz); return; }
      Serial.printf("room ACL (%s):\n", _mesh.nodeName());
      for (int i = 0; i < _room->acl.getNumClients(); i++) {
        ClientInfo* c = _room->acl.getClientByIdx(i);
        if (c->permissions == 0) continue;
        Serial.printf("%02X ", c->permissions);
        mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
        Serial.println();
      }
      reply[0] = 0;
    } else if (strcmp(command, "posts") == 0) {
      if (_room == nullptr) { StrHelper::strncpy(reply, "ERR: not a room slot", reply_sz); return; }
      snprintf(reply, reply_sz, "posts: %u held of %d (RAM only, lost on reboot); members %d",
               (unsigned)_room->num_posted, MAX_UNSYNCED_POSTS, _room->acl.getNumClients());
    } else {
      StrHelper::strncpy(reply, "?", reply_sz);
    }
  }

private:
  // Same 5 s window as the repeater's LAZY_CONTACTS_WRITE_DELAY. 0 means
  // clean, so a deadline that lands exactly on the millis() wrap is nudged.
  void markAclDirty() {
    _acl_dirty_at = millis() + 5000;
    if (_acl_dirty_at == 0) _acl_dirty_at = 1;
  }

  void handleSetPerm(char* args, char* reply, size_t reply_sz) {
    if (_room == nullptr) { StrHelper::strncpy(reply, "ERR: not a room slot", reply_sz); return; }
    char* sp = strchr(args, ' ');
    if (sp == NULL) { StrHelper::strncpy(reply, "ERR: setperm <pubkey-hex> <perms>", reply_sz); return; }
    *sp++ = 0;
    uint8_t pubkey[PUB_KEY_SIZE];
    int hex_len = (int)strlen(args);
    if (hex_len > PUB_KEY_SIZE * 2) hex_len = PUB_KEY_SIZE * 2;
    if (!mesh::Utils::fromHex(pubkey, hex_len / 2, args)) {
      StrHelper::strncpy(reply, "ERR: bad pubkey", reply_sz);
      return;
    }
    uint8_t perms = (uint8_t)atoi(sp);
    if (_room->acl.applyPermissions(_mesh.self_id, pubkey, hex_len / 2, perms)) {
      markAclDirty();   // written by loop(), not here: no flash write in a CLI call
      snprintf(reply, reply_sz, "OK - %d members", _room->acl.getNumClients());
    } else {
      StrHelper::strncpy(reply, "ERR: invalid params", reply_sz);
    }
  }
};
