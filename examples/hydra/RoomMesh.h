#pragma once

// The room server protocol, ported from examples/simple_room_server.
//
// WHERE THIS SITS. RoomMesh is between BaseChatMesh and ChatMesh. A slot that
// is not a room never calls setRoom(), so _room stays null, every override here
// falls through to BaseChatMesh, and a chat slot behaves exactly as before.
// A room slot calls setRoom() and this class takes over the peer layer.
//
// WHY IT TAKES OVER THE PEER LAYER. A chat client finds its peers in a contact
// table. A room server finds them in its ACL. They are different tables with
// different lifetimes, so searchPeersByHash, getPeerSharedSecret,
// onPeerDataRecv, onPeerPathRecv and onAckRecv all switch together on _room.
// This is also why the empty onCommandDataRecv in ChatSlot.h stays empty: that
// callback belongs to the contact path, and a room never reaches it. The room
// answers at onPeerDataRecv, which is where upstream answers.
//
// WHAT THE SHARED RADIO CHANGED. Four things in the upstream room server assume
// that the identity owns the transceiver and the node prefs:
//
//   1. Region scope. Upstream picks a transport scope for a flood reply from
//      its RegionMap, and falls back to a default scope. A slot has no region
//      map yet (decision 9 is not built), so a flood reply here goes out
//      unscoped. BaseChatMesh::sendFloodScoped already does the same thing, so
//      this matches every other client on the node.
//   2. Path hash size. Upstream reads _prefs.path_hash_mode. That is a node
//      setting that slot 0 owns, so a push uses the default size of 1. A REPLY
//      still mirrors the size of the request, which is what matters for a
//      client that is already talking to us.
//   3. Radio statistics. n_packets_recv and n_packets_sent come from the real
//      transceiver, so under one antenna they are NODE totals and not the
//      totals of this identity. Everything else in the status reply is this
//      slot's own: its queue, its airtime, its flood and direct counts, its
//      error flags and its dedup counters. The RSSI and the SNR are the values
//      that the arbiter carried with the last frame it gave to THIS port, and
//      not whatever the modem register holds now.
//   4. Duty cycle. Upstream prices its own airtime. Here SharedRadioCore pools
//      the budget for the whole node, so a busy room cannot spend the share of
//      the repeater. The push interval below is unchanged, and the arbiter
//      decides when the frame actually goes out.

#include "HydraSlot.h"
#include "RoomStore.h"
#include "SlotMeshTables.h"
#include <helpers/BaseChatMesh.h>
#include <target.h>   // board, radio_driver, sensors: node-scoped, one of each

#define ROOM_REPLY_DELAY_MILLIS       1500
#define ROOM_PUSH_NOTIFY_DELAY_MILLIS 2000
#define ROOM_SYNC_PUSH_INTERVAL       1200

#define ROOM_PUSH_ACK_TIMEOUT_FLOOD   12000
#define ROOM_PUSH_TIMEOUT_BASE         4000
#define ROOM_PUSH_ACK_TIMEOUT_FACTOR   2000

#define ROOM_SERVER_RESPONSE_DELAY      300
#define ROOM_TXT_ACK_DELAY              200

#define ROOM_FIRMWARE_VER_LEVEL           1

#define ROOM_REQ_TYPE_GET_STATUS         0x01
#define ROOM_REQ_TYPE_KEEP_ALIVE         0x02
#define ROOM_REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define ROOM_REQ_TYPE_GET_ACCESS_LIST    0x05

#define ROOM_RESP_SERVER_LOGIN_OK           0

// The wire layout of the status reply. The phone app reads these fields in this
// order, so the order is not free to change.
struct RoomServerStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr;    // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint16_t n_posted, n_post_push;
};

class RoomMesh : public BaseChatMesh {
  RoomStore* _room;   // null on a chat slot, and on a room slot that is off
  HydraSlot* _cli;    // where an admin CLI message over the mesh goes

protected:
  RoomMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
           mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _room(nullptr), _cli(nullptr) {}

  // A room finds its peers in the ACL. Its contact table would only collect
  // every advert on the band and never be read.
  bool isAutoAddEnabled() const override { return _room == nullptr; }

  // ------------------------------------------------------------- the peer layer

  int searchPeersByHash(const uint8_t* hash) override {
    if (_room == nullptr) return BaseChatMesh::searchPeersByHash(hash);
    int n = 0;
    for (int i = 0; i < _room->acl.getNumClients(); i++) {
      if (_room->acl.getClientByIdx(i)->id.isHashMatch(hash)) {
        _room->matching_peer_indexes[n++] = i;
      }
    }
    return n;
  }

  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override {
    if (_room == nullptr) { BaseChatMesh::getPeerSharedSecret(dest_secret, peer_idx); return; }
    ClientInfo* c = clientAt(peer_idx);
    // A miss must not leave the buffer of the caller unset. A zero secret fails
    // the MAC check, which is the answer that we want.
    if (c) memcpy(dest_secret, c->shared_secret, PUB_KEY_SIZE);
    else memset(dest_secret, 0, PUB_KEY_SIZE);
  }

  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                      uint8_t* data, size_t len) override {
    if (_room == nullptr) { BaseChatMesh::onPeerDataRecv(packet, type, sender_idx, secret, data, len); return; }
    ClientInfo* client = clientAt(sender_idx);
    if (client == NULL) return;

    if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
      onClientText(packet, client, secret, data, len);
    } else if (type == PAYLOAD_TYPE_REQ && len >= 5) {
      onClientRequest(packet, client, secret, data, len);
    }
  }

  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path,
                      uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override {
    if (_room == nullptr) {
      return BaseChatMesh::onPeerPathRecv(packet, sender_idx, secret, path, path_len, extra_type, extra, extra_len);
    }
    ClientInfo* client = clientAt(sender_idx);
    if (client) {
      client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
      client->last_activity = getRTCClock()->getCurrentTime();
    }
    if (extra_type == PAYLOAD_TYPE_ACK && extra_len >= 4) processRoomAck(extra);
    return false;   // no reciprocal path send
  }

  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override {
    if (_room == nullptr) { BaseChatMesh::onAckRecv(packet, ack_crc); return; }
    if (processRoomAck((uint8_t*)&ack_crc)) packet->markDoNotRetransmit();
  }

  // ------------------------------------------------------------------- the login

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender,
                      uint8_t* data, size_t len) override {
    if (_room == nullptr || packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ) return;

    uint32_t sender_timestamp, sender_sync_since;
    memcpy(&sender_timestamp, data, 4);
    memcpy(&sender_sync_since, &data[4], 4);
    data[len] = 0;

    ClientInfo* client = NULL;
    if (data[8] == 0) {   // a blank password: the sender must already be a member
      client = _room->acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    }
    if (client == NULL) {
      uint8_t perm;
      // A room has no admin password of its own. `password` is a node-level
      // setting (decision 8) and a slot must not carry a second copy of it. An
      // operator grants a room admin by key, with `slot N setperm <key> 3`.
      // That member then logs in with a blank password on the branch above.
      if (strcmp((char*)&data[8], _room->guest_password) == 0) {
        perm = PERM_ACL_READ_WRITE;
      } else if (_room->allow_read_only) {
        perm = PERM_ACL_GUEST;
      } else {
        return;   // no response. The client times out.
      }

      client = _room->acl.putClient(sender, 0);
      if (client == NULL) return;
      if (sender_timestamp <= client->last_timestamp) return;   // replay

      client->last_timestamp = sender_timestamp;
      client->extra.room.sync_since = sender_sync_since;
      client->extra.room.pending_ack = 0;
      client->extra.room.push_failures = 0;
      client->last_activity = getRTCClock()->getCurrentTime();
      client->permissions &= ~PERM_ACL_ROLE_MASK;
      client->permissions |= perm;
      memcpy(client->shared_secret, secret, PUB_KEY_SIZE);
      _room->acl_dirty = true;
    }

    if (packet->isRouteFlood()) client->out_path_len = OUT_PATH_UNKNOWN;

    uint8_t* rd = _room->reply_data;
    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    memcpy(rd, &now, 4);
    rd[4] = ROOM_RESP_SERVER_LOGIN_OK;
    rd[5] = 0;   // legacy: was the recommended keep-alive interval
    rd[6] = (client->isAdmin() ? 1 : (client->permissions == 0 ? 2 : 0));
    rd[7] = client->permissions;
    getRNG()->random(&rd[8], 4);   // a random blob, so the packet hash is unique
    rd[12] = ROOM_FIRMWARE_VER_LEVEL;

    // Give the response time to land before the first push follows it.
    _room->next_push = futureMillis(ROOM_PUSH_NOTIFY_DELAY_MILLIS);

    if (packet->isRouteFlood()) {
      mesh::Packet* path = createPathReturn(sender, client->shared_secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, rd, 13);
      if (path) sendFloodReply(path, ROOM_SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, client->shared_secret, rd, 13);
      if (reply) {
        if (client->out_path_len != OUT_PATH_UNKNOWN) {
          sendDirect(reply, client->out_path, client->out_path_len, ROOM_SERVER_RESPONSE_DELAY);
        } else {
          sendFloodReply(reply, ROOM_SERVER_RESPONSE_DELAY, packet->getPathHashSize());
        }
      }
    }
  }

public:
  // A room slot calls this once, from ChatSlot::begin(). `cli` receives an
  // admin command that arrived over the mesh.
  void setRoom(RoomStore* room, HydraSlot* cli) {
    _room = room;
    _cli = cli;
    // A room with no password admits anybody who sends a blank one, because the
    // two empty strings match. Say so at the moment that the room starts. An
    // operator must not have to read the code to learn it.
    if (_room && _room->guest_password[0] == 0) {
      Serial.println("hydra: room has NO password - anybody can join and post");
    }
  }

  void addSystemPost(const char* text) {
    if (_room == nullptr || text == NULL || text[0] == 0) return;
    storePost(self_id.pub_key, text);
  }

  // The room verbs of `slot N ...`. True means that this function answered.
  // They are not under `set`, which is the same convention as `setperm` and
  // `posts` in ChatSlot.
  bool handleRoomCommand(uint32_t sender_timestamp, const char* command, char* reply, size_t reply_sz) {
    if (_room == nullptr) return false;

    if (strncmp(command, "room.post", 9) == 0) {
      const char* msg = command + 9;
      while (*msg == ' ') msg++;
      if (*msg == 0) StrHelper::strncpy(reply, "ERR: empty message", reply_sz);
      else { addSystemPost(msg); StrHelper::strncpy(reply, "OK", reply_sz); }
      return true;
    }
    if (strncmp(command, "guest.password", 14) == 0) {
      const char* v = command + 14;
      while (*v == ' ') v++;
      if (*v == 0) {
        snprintf(reply, reply_sz, "> %s%s", _room->guest_password,
                 _room->guest_password[0] ? "" : "(none - the room is OPEN)");
      } else {
        StrHelper::strncpy(_room->guest_password, v, sizeof(_room->guest_password));
        _room->acl_dirty = true;
        StrHelper::strncpy(reply, "OK", reply_sz);
      }
      return true;
    }
    if (strncmp(command, "allow.read.only", 15) == 0) {
      const char* v = command + 15;
      while (*v == ' ') v++;
      if (*v == 0) {
        snprintf(reply, reply_sz, "> %s", _room->allow_read_only ? "on" : "off");
      } else {
        _room->allow_read_only = (strncmp(v, "on", 2) == 0) ? 1 : 0;
        _room->acl_dirty = true;
        StrHelper::strncpy(reply, "OK", reply_sz);
      }
      return true;
    }
    return false;
  }

  void loop() {
    BaseChatMesh::loop();
    if (_room == nullptr) return;

    int n = _room->acl.getNumClients();
    if (n <= 0 || !millisHasNowPassed(_room->next_push)) return;

    // A push that got no ACK in time counts as a failure. Clearing pending_ack
    // lets the round-robin offer the SAME post again, because the sync point of
    // the client did not move.
    uint32_t now_ms = (uint32_t)millis();
    for (int i = 0; i < n; i++) {
      ClientInfo* c = _room->acl.getClientByIdx(i);
      if (roomPushAckExpired(c->extra.room.pending_ack, now_ms, (uint32_t)c->extra.room.ack_timeout)) {
        c->extra.room.push_failures++;
        c->extra.room.pending_ack = 0;
      }
    }

    if (_room->next_client_idx >= n) _room->next_client_idx = 0;
    ClientInfo* client = _room->acl.getClientByIdx(_room->next_client_idx);
    bool did_push = false;
    if (roomClientMayPush(client->extra.room.pending_ack, client->last_activity,
                          client->extra.room.push_failures)) {
      uint32_t now = getRTCClock()->getCurrentTime();
      int idx = _room->ring.nextForClient(client->id.pub_key, client->extra.room.sync_since, now);
      if (idx >= 0) {
        pushPostToClient(client, _room->ring.posts[idx]);
        did_push = true;
      }
    }
    _room->next_client_idx = (_room->next_client_idx + 1) % n;

    // Nothing to send to this client, so reach the next one much sooner.
    _room->next_push = futureMillis(did_push ? ROOM_SYNC_PUSH_INTERVAL : ROOM_SYNC_PUSH_INTERVAL / 8);
  }

private:
  ClientInfo* clientAt(int peer_idx) {
    if (peer_idx < 0 || peer_idx >= MAX_CLIENTS) return NULL;
    int i = _room->matching_peer_indexes[peer_idx];
    if (i < 0 || i >= _room->acl.getNumClients()) return NULL;
    return _room->acl.getClientByIdx(i);
  }

  // A slot has no region map, so a flood reply goes out unscoped. See the note
  // at the top of this file.
  void sendFloodReply(mesh::Packet* packet, uint32_t delay_millis, uint8_t path_hash_size) {
    sendFlood(packet, delay_millis, path_hash_size);
  }

  void storePost(const uint8_t* author_key, const char* text) {
    _room->ring.add(author_key, text, getRTCClock()->getCurrentTimeUnique());
    _room->next_push = futureMillis(ROOM_PUSH_NOTIFY_DELAY_MILLIS);
  }

  void pushPostToClient(ClientInfo* client, const PostInfo& post) {
    uint8_t* rd = _room->reply_data;
    int len = 0;
    memcpy(&rd[len], &post.post_timestamp, 4);
    len += 4;   // a PAST timestamp, which the client accepts

    // A retry must have a different packet hash, and therefore a different ACK.
    uint8_t attempt;
    getRNG()->random(&attempt, 1);
    rd[len++] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3);

    memcpy(&rd[len], post.author_key, 4);   // the first 4 bytes name the author
    len += 4;

    int text_len = strlen(post.text);
    memcpy(&rd[len], post.text, text_len);
    len += text_len;

    mesh::Utils::sha256((uint8_t*)&client->extra.room.pending_ack, 4, rd, len,
                        client->id.pub_key, PUB_KEY_SIZE);
    client->extra.room.push_post_timestamp = post.post_timestamp;

    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, client->shared_secret, rd, len);
    if (reply == NULL) {
      client->extra.room.pending_ack = 0;   // nothing went out, so nothing is pending
      return;
    }
    if (client->out_path_len == OUT_PATH_UNKNOWN) {
      sendFlood(reply, (uint32_t)0);
      client->extra.room.ack_timeout = futureMillis(ROOM_PUSH_ACK_TIMEOUT_FLOOD);
    } else {
      sendDirect(reply, client->out_path, client->out_path_len);
      uint8_t hops = client->out_path_len & 63;
      client->extra.room.ack_timeout =
          futureMillis(ROOM_PUSH_TIMEOUT_BASE + ROOM_PUSH_ACK_TIMEOUT_FACTOR * (hops + 1));
    }
    _room->ring.num_post_pushes++;
  }

  // An ACK moves the sync point of the client to the post that we pushed. So
  // the next scan finds the post after it.
  bool processRoomAck(const uint8_t* data) {
    for (int i = 0; i < _room->acl.getNumClients(); i++) {
      ClientInfo* c = _room->acl.getClientByIdx(i);
      if (c->extra.room.pending_ack && memcmp(data, &c->extra.room.pending_ack, 4) == 0) {
        c->extra.room.pending_ack = 0;
        c->extra.room.push_failures = 0;
        c->extra.room.sync_since = c->extra.room.push_post_timestamp;
        return true;
      }
    }
    return false;
  }

  // ------------------------------------------------------- a message from a client

  void onClientText(mesh::Packet* packet, ClientInfo* client, const uint8_t* secret,
                    uint8_t* data, size_t len) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4);
    uint8_t flags = (data[4] >> 2);

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) return;
    if (sender_timestamp < client->last_timestamp) return;   // replay

    bool is_retry = (sender_timestamp == client->last_timestamp);
    client->last_timestamp = sender_timestamp;

    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    client->last_activity = now;
    client->extra.room.push_failures = 0;   // the client is alive, so pushes resume

    data[len] = 0;

    uint32_t ack_hash;
    mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 5 + strlen((char*)&data[5]),
                        client->id.pub_key, PUB_KEY_SIZE);

    uint8_t temp[166];
    temp[5] = 0;
    bool send_ack = false;
    if (flags == TXT_TYPE_CLI_DATA) {
      // An admin runs the slot CLI over the mesh. A retry gets no second run:
      // the command already ran, and running it twice could change state twice.
      if (client->isAdmin() && !is_retry && _cli) {
        _cli->handleCommand(sender_timestamp, (char*)&data[5], (char*)&temp[5], sizeof(temp) - 5);
        temp[4] = (TXT_TYPE_CLI_DATA << 2);
      }
    } else {   // TXT_TYPE_PLAIN: a new post
      if ((client->permissions & PERM_ACL_ROLE_MASK) != PERM_ACL_GUEST) {
        if (!is_retry) storePost(client->id.pub_key, (const char*)&data[5]);
        send_ack = true;   // the ACK is the whole reply
      }
    }

    uint32_t delay_millis = 0;
    if (send_ack) {
      if (client->out_path_len == OUT_PATH_UNKNOWN) {
        mesh::Packet* ack = createAck(ack_hash);
        if (ack) sendFloodReply(ack, ROOM_TXT_ACK_DELAY, packet->getPathHashSize());
        delay_millis = ROOM_TXT_ACK_DELAY + ROOM_REPLY_DELAY_MILLIS;
      } else {
        uint32_t d = ROOM_TXT_ACK_DELAY;
        if (getExtraAckTransmitCount() > 0) {
          mesh::Packet* a1 = createMultiAck(ack_hash, 1);
          if (a1) sendDirect(a1, client->out_path, client->out_path_len, d);
          d += 300;
        }
        mesh::Packet* a2 = createAck(ack_hash);
        if (a2) sendDirect(a2, client->out_path, client->out_path_len, d);
        delay_millis = d + ROOM_REPLY_DELAY_MILLIS;
      }
    }

    int text_len = strlen((char*)&temp[5]);
    if (text_len > 0) {
      // The two timestamps must differ, or the CLI view of the client hides one.
      if (now == sender_timestamp) now++;
      memcpy(temp, &now, 4);

      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
      if (reply) {
        if (client->out_path_len == OUT_PATH_UNKNOWN) {
          sendFloodReply(reply, delay_millis + ROOM_SERVER_RESPONSE_DELAY, packet->getPathHashSize());
        } else {
          sendDirect(reply, client->out_path, client->out_path_len, delay_millis + ROOM_SERVER_RESPONSE_DELAY);
        }
      }
    }
  }

  // -------------------------------------------------------- a request from a client

  void onClientRequest(mesh::Packet* packet, ClientInfo* client, const uint8_t* secret,
                       uint8_t* data, size_t len) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4);
    if (sender_timestamp < client->last_timestamp) return;   // replay
    client->last_timestamp = sender_timestamp;

    client->last_activity = getRTCClock()->getCurrentTime();
    client->extra.room.push_failures = 0;

    if (data[4] == ROOM_REQ_TYPE_KEEP_ALIVE && packet->isRouteDirect()) {
      uint32_t forceSince = 0;
      if (len >= 9) {
        memcpy(&forceSince, &data[5], 4);   // this can be 0, from decrypted padding
      } else {
        memcpy(&data[5], &forceSince, 4);   // zero the bytes that the ACK hash covers
      }
      if (forceSince > 0) client->extra.room.sync_since = forceSince;
      client->extra.room.pending_ack = 0;

      // A keep-alive answer only ever goes DIRECT.
      if (client->out_path_len != OUT_PATH_UNKNOWN) {
        uint32_t ack_hash;
        mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 9, client->id.pub_key, PUB_KEY_SIZE);
        mesh::Packet* reply = createAck(ack_hash);
        if (reply) {
          reply->payload[reply->payload_len++] =
              _room->ring.unsyncedCount(client->id.pub_key, client->extra.room.sync_since);
          sendDirect(reply, client->out_path, client->out_path_len, ROOM_SERVER_RESPONSE_DELAY);
        }
      }
      return;
    }

    int reply_len = handleRequest(client, sender_timestamp, &data[4], len - 4);
    if (reply_len <= 0) return;

    if (packet->isRouteFlood()) {
      mesh::Packet* path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, _room->reply_data, reply_len);
      if (path) sendFloodReply(path, ROOM_SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, _room->reply_data, reply_len);
      if (reply) {
        if (client->out_path_len != OUT_PATH_UNKNOWN) {
          sendDirect(reply, client->out_path, client->out_path_len, ROOM_SERVER_RESPONSE_DELAY);
        } else {
          sendFloodReply(reply, ROOM_SERVER_RESPONSE_DELAY, packet->getPathHashSize());
        }
      }
    }
  }

  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len) {
    uint8_t* rd = _room->reply_data;
    memcpy(rd, &sender_timestamp, 4);   // reflect the timestamp back, as a tag

    if (payload[0] == ROOM_REQ_TYPE_GET_STATUS) {
      RoomServerStats stats;
      stats.batt_milli_volts = board.getBattMilliVolts();
      stats.curr_tx_queue_len = _mgr->getOutboundTotal();
      stats.noise_floor = (int16_t)_radio->getNoiseFloor();
      stats.last_rssi = (int16_t)_radio->getLastRSSI();
      // NODE totals: one transceiver serves every slot.
      stats.n_packets_recv = radio_driver.getPacketsRecv();
      stats.n_packets_sent = radio_driver.getPacketsSent();
      stats.total_air_time_secs = getTotalAirTime() / 1000;
      // The uptime of the BOARD, and not of this identity. It wraps with
      // millis(), at about 49 days.
      stats.total_up_time_secs = millis() / 1000;
      stats.n_sent_flood = getNumSentFlood();
      stats.n_sent_direct = getNumSentDirect();
      stats.n_recv_flood = getNumRecvFlood();
      stats.n_recv_direct = getNumRecvDirect();
      stats.err_events = _err_flags;
      stats.last_snr = (int16_t)(_radio->getLastSNR() * 4);
      stats.n_direct_dups = (uint16_t)((DupCountingTables*)getTables())->getNumDirectDups();
      stats.n_flood_dups = (uint16_t)((DupCountingTables*)getTables())->getNumFloodDups();
      stats.n_posted = _room->ring.num_posted;
      stats.n_post_push = _room->ring.num_post_pushes;

      memcpy(&rd[4], &stats, sizeof(stats));
      return 4 + sizeof(stats);
    }
    if (payload[0] == ROOM_REQ_TYPE_GET_TELEMETRY_DATA) {
      uint8_t perm_mask = ~(payload[1]);
      _room->telemetry.reset();
      _room->telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) perm_mask = 0x00;
      sensors.querySensors(perm_mask, _room->telemetry);

      float temperature = board.getMCUTemperature();
      if (!isnan(temperature)) _room->telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature);

      uint8_t tlen = _room->telemetry.getSize();
      memcpy(&rd[4], _room->telemetry.getBuffer(), tlen);
      return 4 + tlen;
    }
    if (payload[0] == ROOM_REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
      if (payload[1] == 0 && payload[2] == 0) {
        uint8_t ofs = 4;
        for (int i = 0; i < _room->acl.getNumClients() && ofs + 7 <= MAX_PACKET_PAYLOAD - 4; i++) {
          ClientInfo* c = _room->acl.getClientByIdx(i);
          if (!c->isAdmin()) continue;
          memcpy(&rd[ofs], c->id.pub_key, 6); ofs += 6;   // a 6-byte pubkey prefix
          rd[ofs++] = c->permissions;
        }
        return ofs;
      }
    }
    return 0;   // an unknown command
  }
};
