#pragma once

// Makes a node that has no companion role answerable by the MeshCore phone app.
//
// WHAT PROBLEM THIS SOLVES. A repeater or a room server is administered today
// by a LoRa packet: the app logs in over the air, asks for the status over the
// air, and reads the ACL over the air. Every one of those spends airtime on a
// duty-cycled band, and it fails when the operator is standing beside the node
// but the mesh path is down. This class answers the same requests over BLE,
// with no packet on the air.
//
// HOW THE APP FINDS THE NODE. It does not need a change. SerialBLEInterface
// already advertises the Nordic UART service that the app scans for. Before
// this class, the app connected, sent CMD_APP_START, got a text error and gave
// up.
//
// THE CONTACT TRICK. The app administers a repeater as a CONTACT, not as the
// radio it is connected to. This is verified: the reference client
// meshcore.js finds the repeater with findContactByPublicKeyPrefix() and then
// calls login(contact.publicKey, ...) and getStatus(contact.publicKey). So the
// node reports one synthetic contact for each identity that it hosts. The
// operator then taps that contact in the app and gets the usual repeater
// screens.
//
// THE LOOPBACK. CMD_SEND_LOGIN, CMD_SEND_STATUS_REQ and CMD_SEND_BINARY_REQ
// each name a destination public key. When that key is one of ours, the facade
// answers from memory and gives back a frame with the shape of one that came in
// over LoRa. When the key is not ours, the facade replies "not found": it has
// no contact table and cannot send to a third party.
//
// PER-IDENTITY AUTHORITY. A login authorises ONE slot. The session permissions
// are held for each slot on its own, so a login against slot 1 grants nothing
// on slot 2. The session lives only as long as the BLE connection.
//
// ONE CONNECTION, TWO PROTOCOLS. The same link carries the text CLI. See
// companion::isCompanionFrame() in CompanionFrames.h for the rule that tells
// the two apart.

#include "CompanionFrames.h"
#include <helpers/BaseSerialInterface.h>

#ifndef COMPANION_MAX_SLOTS
  #define COMPANION_MAX_SLOTS  8
#endif

// The MyMesh CLI writes into a reply buffer with no bound. 160 bytes is the
// size that examples/hydra/main.cpp gives it. Do not make this smaller.
#ifndef COMPANION_CLI_REPLY_MAX
  #define COMPANION_CLI_REPLY_MAX  160
#endif

namespace companion {

// What the node tells the facade about itself. The facade never reads mesh
// state on its own, so it stays free of any one node role.
//
// The node implements this. examples/hydra/HydraCompanion.h is the hydra
// implementation.
class Host {
public:
  virtual ~Host() {}

  // --- the node
  virtual void getDeviceInfo(DeviceInfo& out) = 0;
  virtual void getSelfInfo(SelfInfo& out) = 0;
  virtual uint32_t getCurrentTime() = 0;
  // False means that the facade must reply with an error. Upstream refuses a
  // time that runs the clock backwards.
  virtual bool setCurrentTime(uint32_t secs) = 0;
  virtual uint16_t getBattMilliVolts() = 0;
  virtual void getStorageKb(uint32_t& used, uint32_t& total) = 0;
  // The feature level that a repeater reports in a login reply. Upstream
  // examples/simple_repeater/MyMesh.cpp calls it FIRMWARE_VER_LEVEL and sets 2.
  virtual uint8_t getFirmwareVerLevel() { return 2; }

  // --- the identities
  /* THE INDEX SPACE. getSlotCount() is the number of slot INDICES, and it does
     not change while the node runs. A slot that is off keeps its index and
     getSlotContact() gives back false for it. The facade holds the session
     permissions in an array with that index, so an index that moved when an
     operator turned a slot off would carry a login from one identity to
     another. */
  virtual int getSlotCount() = 0;
  virtual bool getSlotContact(int idx, Contact& out) = 0;
  // How many of those slots are enabled now. CONTACTS_START carries this.
  virtual int getEnabledSlotCount() = 0;
  // The index of the slot with this key, or -1 when the key is not ours.
  virtual int findSlotByPubKey(const uint8_t* pub_key) = 0;
  // The password rule of the slot. Each slot holds its own passwords.
  virtual bool slotLogin(int idx, const char* password, uint8_t& perms) = 0;
  // Answers a request that came in with the given session permissions. The
  // return value is the length written to `dest`, or -1 when the slot does not
  // answer this request type.
  virtual int slotRequest(int idx, uint8_t perms, const uint8_t* req, size_t req_len,
                          uint8_t* dest, size_t max_len) = 0;

  // --- the text CLI on the same link
  // `command` is a writable, terminated string. The implementation writes the
  // answer into `reply`. A sender_timestamp that is not 0 means that the caller
  // is not at the serial console, which is the convention of upstream.
  virtual void handleCliCommand(uint32_t sender_timestamp, char* command,
                                char* reply, size_t reply_sz) = 0;
};

class CompanionFacade {
public:
  CompanionFacade(BaseSerialInterface& serial, Host& host);

  // Call once for each pass of the main loop. It reads at most one frame and it
  // sends at most one contact of a list that is part way through.
  void loop();

private:
  void onDisconnect();
  void handleFrame(uint8_t* frame, size_t len);
  void handleCli(uint8_t* frame, size_t len);
  void pumpContacts();

  void sendErr(uint8_t code);
  // Answers one of the three commands that name a destination public key.
  void handleLogin(const uint8_t* frame, size_t len);
  void handleStatusReq(const uint8_t* frame, size_t len);
  void handleBinaryReq(const uint8_t* frame, size_t len);
  uint32_t nextTag();

  BaseSerialInterface* _serial;
  Host* _host;

  bool     _connected;
  uint32_t _tag_seq;

  // The session. 0xFF in _perms means "this slot has had no login".
  uint8_t  _perms[COMPANION_MAX_SLOTS];

  // The contact list runs across several passes of the loop. The send queue of
  // SerialBLEInterface holds 12 frames, and a node with 8 slots would need 10
  // in one pass. One frame for each pass never fills it.
  bool     _iter_active;
  int      _iter_idx;
  uint32_t _iter_since;
  uint32_t _iter_most_recent;

  uint8_t  _in[MAX_FRAME_SIZE + 1];    // +1 so a text command can be terminated
  uint8_t  _out[COMP_MAX_FRAME];
  char     _cli_reply[COMPANION_CLI_REPLY_MAX];
};

}   // namespace companion
