#pragma once

// Slots 1..N: a chat identity.
//
// SCAFFOLDING. This boots, gets its own keypair, adverts, and tracks contacts —
// enough to prove a second identity can live on the shared radio and be seen by
// the mesh as a separate node. It has no companion interface and answers no
// messages; the message/ack callbacks are deliberately empty. The diagnostic
// chat bot and the BLE companion facade both hang off ChatMesh below.

#include "HydraSlot.h"
#include "SlotMeshTables.h"
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
#ifndef HYDRA_NAME_PREFIX
  #define HYDRA_NAME_PREFIX  "hydra"
#endif

class ChatMesh : public BaseChatMesh {
  char _name[32];
  unsigned long _next_advert;

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
      : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _next_advert(0) {
    _name[0] = 0;
  }

  const char* nodeName() const { return _name; }
  void setNodeName(const char* n) { StrHelper::strncpy(_name, n, sizeof(_name)); }
  bool hasPendingWork() const { return _mgr->getOutboundTotal() > 0; }

  void advertise(uint32_t delay_millis, bool flood) {
    mesh::Packet* pkt = createSelfAdvert(_name);
    if (pkt == NULL) return;
    if (flood) sendFlood(pkt, delay_millis); else sendZeroHop(pkt, delay_millis);
    _next_advert = futureMillis((int)(HYDRA_CHAT_ADVERT_MINS * 60000UL));
  }

  void loop() {
    BaseChatMesh::loop();
    // zero until the slot's first advert, so a slot that never started is quiet
    if (HYDRA_CHAT_ADVERT_MINS > 0 && _next_advert != 0 && millisHasNowPassed(_next_advert)) {
      advertise(0, false);
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
  SlotType                        _type;
  bool                            _begun;

public:
  ChatSlot()
      : _mgr(HYDRA_CHAT_POOL), _mesh(_port, _ms, _rng, rtc_clock, _mgr, _tables),
        _type(SLOT_OFF), _begun(false) {}

  RadioPort& port() override { return _port; }
  SlotType type() const override { return _type; }
  const mesh::LocalIdentity& identity() const override { return _mesh.self_id; }
  bool hasPendingWork() const override { return _begun && _mesh.hasPendingWork(); }
  ChatMesh& mesh() { return _mesh; }   // hook for the companion facade / diag bot

  bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name) override {
    if (_begun) return true;
    _mgr.allocatePool();
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
    char nm[32];   // "_slot1" is the storage key; the mesh sees "hydra slot1"
    snprintf(nm, sizeof(nm), "%s %s", HYDRA_NAME_PREFIX, id_name[0] == '_' ? id_name + 1 : id_name);
    _mesh.setNodeName(nm);
    // Only the base begin(). Unlike the repeater's MyMesh::begin(), a chat slot
    // pushes no freq/bw/sf/cr or TX power at the radio — those are node
    // settings, owned by slot 0's prefs, and there is one transceiver.
    _mesh.begin();
    _type = SLOT_CHAT;
    _begun = true;
    _mesh.advertise(8000, false);
    return true;
  }

  void loop() override { if (_begun) _mesh.loop(); }

  void handleCommand(char* command, char* reply, size_t reply_sz) override {
    if (!_begun) { StrHelper::strncpy(reply, "slot not running", reply_sz); return; }
    if (strcmp(command, "advert") == 0) {
      _mesh.advertise(0, false);
      StrHelper::strncpy(reply, "OK - zero-hop advert queued", reply_sz);
    } else if (strcmp(command, "flood advert") == 0) {
      _mesh.advertise(0, true);
      StrHelper::strncpy(reply, "OK - flood advert queued", reply_sz);
    } else if (strcmp(command, "contacts") == 0) {
      snprintf(reply, reply_sz, "contacts: %d", _mesh.getNumContacts());
    } else {
      StrHelper::strncpy(reply, "?", reply_sz);
    }
  }
};
