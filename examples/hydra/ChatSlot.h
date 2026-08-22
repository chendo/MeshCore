#pragma once

// Slots 1..N hold an identity that is not the repeater. It is a chat client or
// a room server. One class covers both, because everything below the ACL and
// the post buffer is the same: a keypair, a name, contacts, adverts, and no
// packet forward.
//
// THIS IS A FRAME FOR LATER WORK. The slot boots, gets its own keypair, adverts
// and keeps a list of contacts. That is enough to prove that a second identity
// can live on the shared radio, and that the mesh sees it as a separate node.
// The slot has no companion interface, and it answers no messages. The
// callbacks for a message and for an ack are empty on purpose.
//
// A room slot gets a real, private ClientACL, a real post buffer and the room
// server protocol. RoomMesh.h holds that protocol, and RoomSync.h holds the part
// of it that a host test can run. A room slot never reaches onCommandDataRecv
// below: that callback belongs to the contact path of a chat client, and a room
// answers one level down, at onPeerDataRecv. RoomMesh.h says why.

#include "DiagBot.h"
#include "HydraSlot.h"
#include "RoomMesh.h"
#include "RoomStore.h"
#include "SlotMeshTables.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <target.h>   // board, radio_driver, rtc_clock: node-scoped, one of each

// A slot that is not the repeater gets a much smaller size. sizeof(mesh::Packet)
// is 262 B, so the 32-entry pool of the repeater alone is ~8.3 KB. A chat
// identity only ever has its own traffic in flight, because it forwards
// nothing. So 8 entries are more than enough.
#ifndef HYDRA_CHAT_POOL
  #define HYDRA_CHAT_POOL     8
#endif
#ifndef HYDRA_CHAT_HASHES
  #define HYDRA_CHAT_HASHES  32
#endif

#ifndef HYDRA_CHAT_ADVERT_MINS
  #define HYDRA_CHAT_ADVERT_MINS  60
#endif

// The first advert of a slot waits 8 s for the radio to settle. Every slot on
// the board starts in the same pass of setup(), so that one delay puts all of
// them on the air together. This window spreads that first advert by key, in
// the same way as nextAdvertDelay spreads every advert after it.
#ifndef HYDRA_ADVERT_START_MS
  #define HYDRA_ADVERT_START_MS      8000
#endif
#ifndef HYDRA_ADVERT_START_SPREAD_MS
  #define HYDRA_ADVERT_START_SPREAD_MS  30000
#endif

class ChatMesh : public RoomMesh {

  char _name[SLOT_NAME_MAX];
  unsigned long _next_advert;
  uint8_t _advert_mins;
  bool _flood;
  uint8_t _adv_type;
  DiagBot _diag;   // off until `slot N set diag on`; see DiagBot.h

protected:
  float getAirtimeBudgetFactor() const override { return 1.0f; }
  int calcRxDelay(float score, uint32_t air_time) const override { return 0; }

  // A chat identity is an end point, not a relay. Slot 0 already forwards
  // everything that this radio hears. If a second identity forwarded the same
  // floods, one antenna would put two copies of each flood on the air.
  bool allowPacketForward(const mesh::Packet* packet) override { return false; }

  void onDiscoveredContact(ContactInfo& c, bool is_new, uint8_t path_len, const uint8_t* path) override {}
  void onContactPathUpdated(const ContactInfo& c) override {}
  ContactInfo* processAck(const uint8_t* data) override { return NULL; }
  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t ts, const char* text) override { _diag.onText(*this, from, pkt, ts, text); }
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
  void onTraceRecv(mesh::Packet* pkt, uint32_t tag, uint32_t auth, uint8_t flags, const uint8_t* snrs, const uint8_t* hashes, uint8_t path_len) override { _diag.onTraceResult(*this, pkt, tag, auth, flags, snrs, hashes, path_len); }

public:
  ChatMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
           mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
      // A slot floods its advert by default. A slot that nobody can find is
      // not useful. The operator does not have to know to enable discovery.
      // The value _advert_mins limits the airtime. It is the setting that
      // matters.
      : RoomMesh(radio, ms, rng, rtc, mgr, tables), _next_advert(0),
        _advert_mins(HYDRA_CHAT_ADVERT_MINS), _flood(true), _adv_type(ADV_TYPE_CHAT) {
    _name[0] = 0;
  }

  const char* nodeName() const { return _name; }
  void setNodeName(const char* n) { StrHelper::strncpy(_name, n, sizeof(_name)); }
  void setAdvertType(uint8_t t) { _adv_type = t; }
  bool hasPendingWork() const { return _mgr->getOutboundTotal() > 0; }
  DiagBot& diag() { return _diag; }

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
    // Decision C. The interval starts when you enable the slot. So the slot
    // announces itself at that moment. It does not wait for the first cycle.
    // nextAdvertDelay then holds this slot at its own point of the cycle, so
    // the identities of one board do not transmit together. It returns 0 only
    // for an interval of 0, which is the setting for "never advert".
    uint32_t d = nextAdvertDelay((uint32_t)_ms->getMillis(), _advert_mins, self_id.pub_key, PUB_KEY_SIZE,
                                 getRNG()->nextInt(0, HYDRA_ADVERT_JITTER_MS));
    _next_advert = d ? futureMillis((int)d) : 0;
  }

  void loop() {
    RoomMesh::loop();
    // this is zero until the first advert. A slot that never started stays quiet.
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
  RoomStore*                      _room;   // not null only while a room slot runs
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
  ChatMesh& mesh() { return _mesh; }   // a hook for the companion or the diag bot
  RoomStore* room() { return _room; }

  // This is what the slot will ask the heap for, before it asks. It is a report
  // only. The real gate is RamFloor. RamFloor pins the reserve, and then lets
  // these allocations use what is left.
  static size_t heapCost(SlotType t) {
    size_t n = (size_t)HYDRA_CHAT_POOL * sizeof(mesh::Packet) + 256;
    if (t == SLOT_ROOM) n += RoomStore::heapCost();
    return n;
  }

  bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name,
             const char* display_name, SlotType type) override {
    if (_begun) return true;
    if (!_mgr.allocatePool()) return false;   // the heap refused: the RAM floor works
    _fs = fs;
    if (!store.load(id_name, _mesh.self_id)) {
      // radio_new_identity() samples the RSSI noise. This takes the transceiver
      // out of receive. It happens only at the first boot of a slot. The next
      // pump() puts the radio back in receive. So the node is deaf for one
      // iteration of the loop.
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
      if (_room == nullptr) return false;   // as above: refuse, do not start a part
      // The scope is the STORAGE name of the slot. That name is fixed at
      // creation and does not follow the type (decision 7). So a slot that
      // changes its type keeps its members.
      snprintf(_acl_file, sizeof(_acl_file), "/acl%s", id_name);
      _room->load(_fs, _mesh.self_id, _acl_file);
      _mesh.setRoom(_room, this);   // from here the mesh answers as a room server
    }

    // This calls only the base begin(). MyMesh::begin() in the repeater sends
    // freq, bw, sf, cr and the TX power to the radio. A chat slot sends none of
    // them. They are node settings, the prefs of slot 0 own them, and there is
    // one transceiver.
    _mesh.begin();
    _type = type;
    _begun = true;
    _mesh.advertise(HYDRA_ADVERT_START_MS +
                    advertPhaseWithin(HYDRA_ADVERT_START_SPREAD_MS, _mesh.self_id.pub_key, PUB_KEY_SIZE),
                    _mesh.floodAdvert());
    return true;
  }

  // When you disable a slot, the code silences its port. It does not free the
  // room. The slot must be able to come back without a continuous block from
  // the heap months later. This code also does not promise that _mesh.begin()
  // can run a second time. This function writes the ACL, so that nothing
  // pending is lost.
  void flushPendingWrites() override {
    if (_room && _acl_dirty_at && _fs) {
      _room->save(_fs, _acl_file);
      _acl_dirty_at = 0;
    }
  }

  void loop() override {
    if (!_begun) return;
    _mesh.loop();
    // The code groups these writes in the same way as the repeater groups its
    // ACL writes. The reason is the same. The write blocks the loop for ~1.6 s
    // and wears the flash. So a series of `setperm` commands costs one write,
    // and not one write for each command.
    if (_room && _room->acl_dirty) { _room->acl_dirty = false; markAclDirty(); }
    if (_acl_dirty_at && (long)(millis() - _acl_dirty_at) >= 0) flushPendingWrites();
  }

  void handleCommand(uint32_t sender_timestamp, char* command,
                     char* reply, size_t reply_sz) override {
    if (!_begun) { StrHelper::strncpy(reply, "slot not running", reply_sz); return; }

    if (_mesh.handleRoomCommand(sender_timestamp, command, reply, reply_sz)) return;

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
      // ClientACL deletes a client when the code changes that client to a guest.
      // Stop if the count ever fails to fall. If the code did not stop, the loop
      // watchdog would reboot the node.
      for (int n = _room->acl.getNumClients(); n > 0; n = _room->acl.getNumClients()) {
        ClientInfo* c = _room->acl.getClientByIdx(0);
        _room->acl.applyPermissions(_mesh.self_id, c->id.pub_key, PUB_KEY_SIZE, PERM_ACL_GUEST);
        if (_room->acl.getNumClients() >= n) break;
      }
      markAclDirty();
      StrHelper::strncpy(reply, "OK - room ACL cleared", reply_sz);
    } else if (sender_timestamp == 0 && strcmp(command, "acl") == 0) {
      // The user reaches this as `slot N get acl`. HydraNode removes the "get ".
      // This is for the serial console only, like `get acl` on the repeater. It
      // lists third parties.
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
      snprintf(reply, reply_sz, "posts: %u stored, %u pushed; buffer %d (RAM only, lost on reboot); members %d",
               (unsigned)_room->ring.num_posted, (unsigned)_room->ring.num_post_pushes,
               MAX_UNSYNCED_POSTS, _room->acl.getNumClients());
    } else {
      StrHelper::strncpy(reply, "?", reply_sz);
    }
  }

private:
  // This is the same 5 s window as LAZY_CONTACTS_WRITE_DELAY in the repeater. A
  // value of 0 means clean. So the code moves a deadline that falls exactly on
  // the millis() wrap.
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
      markAclDirty();   // loop() writes this, not here. A CLI call does no flash write.
      snprintf(reply, reply_sz, "OK - %d members", _room->acl.getNumClients());
    } else {
      StrHelper::strncpy(reply, "ERR: invalid params", reply_sz);
    }
  }
};
