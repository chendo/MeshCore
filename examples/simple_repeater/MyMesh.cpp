#include "MyMesh.h"
#include <algorithm>

#if WITH_STATUS_LED
#include "helpers/StatusLed.h"
#endif

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#elif WITH_BLE_BRIDGE
    reply_data[8] |= 0x05;  // is bridge, type BLE
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  TransportKey req_scope;
  bool is_wildcard = recv_pkt_region != NULL && recv_pkt_region->isWildcard();
  bool req_scope_known = recv_pkt_region != NULL && !is_wildcard
                      && region_map.getTransportKeysFor(*recv_pkt_region, &req_scope, 1) > 0;

  switch (mesh::chooseReplyScope(req_scope_known, is_wildcard, !default_scope.isNull())) {
    case mesh::REPLY_SCOPE_REQUEST:
      sendFloodScoped(req_scope, packet, delay_millis, path_hash_size);   // reply with same scope as request
      break;
    case mesh::REPLY_SCOPE_DEFAULT:
      // requester's scope is unknown: DIRECT request (no transport codes), or code matched no Region.
      // un-scoped would be dropped at hop 0 by repeaters running flood.max.unscoped=0
      sendFloodScoped(default_scope, packet, delay_millis, path_hash_size);
      break;
    case mesh::REPLY_SCOPE_NONE:
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
      break;
  }
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()
      && mesh::isFloodHopLimitExceeded(packet, _prefs.flood_max, _prefs.flood_max_unscoped, _prefs.flood_max_advert)) {
    return false;
  }
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if WITH_STATUS_LED
  StatusLed::loraRx();
#if WITH_MESH_OBSERVER
  _obs.observeRx(raw, len, (int8_t)(snr * 4));   // peers, hops, types, relay confirms
#endif
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  /* RECEIVE side of the bridge (modes rx and both).
     This is what makes the two bridge nodes behave as though they were sitting
     next to each other: everything this node HEARS on its band is offered to
     the other one. It has to be what we heard, not merely what we chose to
     relay -- our routing policy drops duplicates and hop-exceeded packets
     because they are old news ON THIS BAND, while the far band may never have
     seen them at all. Dedup is per-band; the bridge crosses bands.

     APPEND OURSELVES TO THE PATH FIRST. logRx runs at Dispatcher.cpp:238,
     BEFORE routeRecvPacket() appends this node's hash at Mesh.cpp:349 -- so the
     copy we bridge would otherwise describe a route that never mentions us, and
     the far side would relay it as though the packet had arrived from thin air.
     The bridge would be a tunnel, not a hop.

     That is not cosmetic. The path IS the return route for direct packets, and
     the bridge is the only link between the two bands: omit ourselves and a
     reply is routed back through hops that cannot carry it. Flood traffic
     survives because it is broadcast; anything direct does not.

     The later logTx copy carries the same hash but is dropped by the bridge's
     own dedup -- calculatePacketHash() covers the payload and not the path, so
     both copies hash alike and the first one through wins. Making that first
     copy the correct one is the whole fix.

     Restore the count afterwards so local processing is untouched; only the
     count bits change, and hash bytes past the count are ignored. Floods only:
     a direct packet follows a fixed path that we must not rewrite. */
  if (_prefs.bridge_pkt_src >= 1) {
    const uint8_t n = pkt->getPathHashCount();
    const uint8_t hsz = pkt->getPathHashSize();
    bool appended = false;
    if (pkt->isRouteFlood() && (n + 1) * hsz <= MAX_PATH_SIZE) {
      self_id.copyHashTo(&pkt->path[n * hsz], hsz);
      pkt->setPathHashCount(n + 1);
      appended = true;
    }
    bridge.sendPacket(pkt);
    if (appended) pkt->setPathHashCount(n);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#if WITH_STATUS_LED
  StatusLed::loraTx();
#if WITH_MESH_OBSERVER
  // only the header byte matters here: observeTx uses it to spot floods, which
  // are the only transmissions that can come back to us relayed
  { uint8_t hdr = pkt->header; _obs.observeTx(&hdr, 1); }
#endif
#ifdef WITH_BRIDGE
  /* TRANSMIT side of the bridge (modes tx and both).
     Without this the node itself is unreachable across the bridge: it can be
     addressed, but its adverts and its replies are transmissions rather than
     receptions, so they never cross and the answer never comes back.

     This also re-offers packets we relayed after receiving them over the
     bridge. That echo is bounded, not a loop -- BridgeBase::_seen_packets at
     the far end has already marked them and drops them on arrival, which is
     what its dup counter has been recording all along. One wasted crossing per
     packet, and the loop terminates. */
  if (_prefs.bridge_pkt_src != 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  return Mesh::onRecvPacket(pkt);
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = 0xFF;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    // a DIRECT login can reply via the stored out_path, as onPeerDataRecv() does for REQ
    ClientInfo* client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    bool have_out_path = client != NULL && client->out_path_len != OUT_PATH_UNKNOWN;

    auto route = mesh::chooseReplyRoute(packet->isRouteFlood(), reply_path_len != 0xFF, have_out_path);

    if (route == mesh::REPLY_ROUTE_PATH_RETURN) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      return;
    }

    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
    if (reply == NULL) return;

    if (route == mesh::REPLY_ROUTE_DIRECT_SUPPLIED) {
      sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    } else if (route == mesh::REPLY_ROUTE_DIRECT_OUT_PATH) {
      sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
    } else {
      sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->getPathHashCount() == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
#if defined(WITH_BLE_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
  recv_pkt_region = NULL;

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();

#ifdef LOOP_WATCHDOG_MS
  /* Deliberately NOT tightened to LOOP_WATCHDOG_MS here. main.cpp armed the
     watchdog with the generous boot limit before any of this ran, and the
     remainder of begin() -- loadPrefs, acl.load, region_map.load, then the
     SoftDevice role ladder in startBLE which can cycle Bluefruit.begin()
     several times -- still has to complete before the main loop ever runs.
     Imposing the 30s runtime limit at this point would reset the node partway
     through a slow-but-legitimate boot and loop it forever. The limit tightens
     on the first loop pass, once there is a loop to watch. */
#endif
  /* Capacity is the one thing the estimator cannot infer, so it has to be
     told. 0 leaves current and power unreported rather than guessed. */
#ifndef BATTERY_CAPACITY_MAH
  #define BATTERY_CAPACITY_MAH 0
#endif
  _batt.begin(BATTERY_CAPACITY_MAH);

#if WITH_MESH_OBSERVER
  // The observer cannot recognise a relay of OUR OWN transmission without
  // knowing our key: it looks for our hash in the paths of packets we overhear.
  _obs.addSelfKey(self_id.pub_key);
  // Without this the observer holds no clock, so every advert timestamp is
  // discarded on the null check and clock readings never happen at all.
  _obs.setClock(getRTCClock());
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(_prefs.radio_fem_txgain);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

#if defined(USE_LR2021)
bool MyMesh::configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) {
  return radio_driver.configSideDetectors(sideDetSFs, num, bw);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

#if defined(WITH_BLE_BRIDGE)
#if WITH_MESH_OBSERVER
void MyMesh::onClockSetExternally() {
  _clock_extern_set_ms = millis();
  _clock_ever_set = true;
}

bool MyMesh::clockIsUnset() const {
  return getRTCClock()->getCurrentTime() < MeshObserver::CLOCK_SET_EPOCH;
}

/**
 * @brief  Steer our clock towards the zero-hop neighbourhood's consensus.
 *
 * The estimator lives in MeshObserver; everything here is policy, and it is
 * deliberately asymmetric. MeshCore itself refuses to move a clock backwards
 * ("clock sync" and "time" both do), because the replay defences -- a contact's
 * last_advert_timestamp, a client's last_timestamp -- assume our timestamps
 * only ever increase. So:
 *
 *   behind, and the neighbourhood is emphatic  -> step the whole way at once
 *   behind slightly, or ahead at all           -> slew, a couple of seconds a time
 *
 * Stepping forward is the just-rebooted case, where slewing would take days to
 * close a gap of minutes. Stepping backward is never allowed: the most we do
 * when we are ahead is bleed it off slowly, and even that is a small
 * monotonicity violation, so it is capped hard.
 */

void MyMesh::maybeConvergeClock() {
  if (!_prefs.clock_converge || !millisHasNowPassed(_next_clock_converge_ms)) return;

  const bool unset = clockIsUnset();
  _next_clock_converge_ms = futureMillis(unset ? CLOCK_CONVERGE_FAST_MS
                                               : CLOCK_CONVERGE_INTERVAL_MS);

  MeshObserver::ClockConsensus cc =
      _obs.clockConsensus(unset ? CLOCK_UNSET_MIN_SOURCES
                                : MeshObserver::CLOCK_MIN_SOURCES);

  if (unset) {
    // Nothing here is worth protecting, so the only question is whether the
    // neighbours agree well enough to be believed at all.
    if (!cc.valid) return;
    if (cc.spread_s > CLOCK_UNSET_MAX_SPREAD_S) return;
    if (cc.offset_s <= 0) return;               // only ever forward out of this
    uint32_t now = getRTCClock()->getCurrentTime();
    getRTCClock()->setCurrentTime((uint32_t)((int64_t)now + cc.offset_s));
    _last_clock_adj_s = cc.offset_s;
    _clock_steps++;
    return;
  }

  // A clock a person or a client app just set beats anything the neighbourhood
  // can offer. The survey this is tuned against found whole sub-networks that
  // agreed with each other and were wrong together by minutes, and a node with
  // good time surrounded by one of those would otherwise be dragged into it.
  if (_clock_ever_set &&
      !millisHasNowPassed(_clock_extern_set_ms + CLOCK_HOLDOVER_MS)) return;

  if (!cc.valid) return;

  const int32_t off = cc.offset_s;            // seconds to ADD to our clock
  if (off >= -CLOCK_DEADBAND_S && off <= CLOCK_DEADBAND_S) return;
  // Sources that disagree this widely are not a measurement of anything.
  if (cc.spread_s > CLOCK_MAX_SPREAD_TO_ACT_S) return;

  int32_t adj;
  if (off >= CLOCK_STEP_MIN_S && cc.agree_pct >= CLOCK_STEP_MIN_AGREE
      && cc.n_used >= CLOCK_STEP_MIN_SOURCES
      && cc.spread_s <= CLOCK_STEP_MAX_SPREAD_S) {
    adj = off;
    _clock_steps++;
  } else {
    adj = (off > 0) ? CLOCK_SLEW_MAX_S : -CLOCK_SLEW_MAX_S;
    if (off > 0 && off < adj) adj = off;      // never overshoot into oscillation
    if (off < 0 && off > adj) adj = off;
    _clock_slews++;
  }

  uint32_t now = getRTCClock()->getCurrentTime();
  getRTCClock()->setCurrentTime((uint32_t)((int64_t)now + adj));
  _last_clock_adj_s = adj;
}
#endif

#if WITH_MESH_OBSERVER
void MyMesh::formatObserverReply(char *reply, const char* what) {
  // 1.17.1 passes no size; the caller's buffer is char reply[160].
  const size_t reply_size = 160;
  size_t o = 0;
  if (strcmp(what, "hops") == 0) {
    // how far away the traffic we hear originates; bucket 0 came straight off
    // the sender's radio, so it is the count of genuinely direct receives
    o += snprintf(reply, reply_size, "hops seen (of %lu frames):",
                  (unsigned long)_obs.framesObserved());
    for (int h = 0; h < MeshObserver::HOP_BUCKETS && o + 10 < reply_size; h++) {
      uint32_t n = _obs.hopCount(h);
      if (n) o += snprintf(reply + o, reply_size - o, " %d:%lu", h, (unsigned long)n);
    }
  } else if (strcmp(what, "types") == 0) {
    static const char* T[16] = {"REQ","RESP","TXT","ACK","ADV","GTXT","GDAT","ANON",
                                "PATH","TRACE","MPART","CTRL","?","?","?","RAW"};
    o += snprintf(reply, reply_size, "types:");
    for (int t = 0; t < 16 && o + 12 < reply_size; t++) {
      uint32_t n = _obs.typeCount(t);
      if (n) o += snprintf(reply + o, reply_size - o, " %s:%lu", T[t], (unsigned long)n);
    }
  } else if (strcmp(what, "heard") == 0) {
    // relay confirmation: proof our transmissions are actually being received
    uint32_t sent = _obs.floodsSent(), conf = _obs.floodsConfirmed();
    snprintf(reply, reply_size,
             "floods sent %lu, confirmed relayed %lu (%lu%%); by hash width 2B:%lu 1B:%lu(ignored); window %lums",
             (unsigned long)sent, (unsigned long)conf,
             (unsigned long)(sent ? conf * 100 / sent : 0),
             (unsigned long)_obs.confirmsByWidth(2), (unsigned long)_obs.confirmsByWidth(1),
             (unsigned long)_obs.confirmWindow());
  } else if (memcmp(what, "clocks", 6) == 0) {
    const char* arg = (what[6] == ' ') ? &what[7] : "";
    if (memcmp(arg, "on", 2) == 0) {
      _prefs.clock_converge = 1;
      _next_clock_converge_ms = futureMillis(CLOCK_CONVERGE_INTERVAL_MS);
      savePrefs();
    } else if (memcmp(arg, "off", 3) == 0) {
      _prefs.clock_converge = 0;
      savePrefs();
    }
    uint32_t hold_m = 0;
    if (_clock_ever_set) {
      uint32_t since = millis() - _clock_extern_set_ms;
      if (since < CLOCK_HOLDOVER_MS) hold_m = (CLOCK_HOLDOVER_MS - since) / 60000;
    }
    const uint8_t min_src = clockIsUnset() ? CLOCK_UNSET_MIN_SOURCES
                                           : MeshObserver::CLOCK_MIN_SOURCES;
    MeshObserver::ClockConsensus cc = _obs.clockConsensus(min_src);
    if (cc.valid) {
      snprintf(reply, reply_size,
               "clocks %s%s: %+ds from %u/%u src (%u direct, %u%%, spread %ds); hop %ums/%lup; step %lu slew %lu last %+ds; hold %lum",
               _prefs.clock_converge ? "on" : "off", clockIsUnset() ? " UNSET" : "",
               (int)cc.offset_s, cc.n_used, cc.n_seen,
               cc.n_zero_hop, cc.agree_pct, (int)cc.spread_s,
               cc.hop_delay_ms, (unsigned long)_obs.hopDelayPairs(),
               (unsigned long)_clock_steps, (unsigned long)_clock_slews,
               (int)_last_clock_adj_s, (unsigned long)hold_m);
    } else {
      snprintf(reply, reply_size,
               "clocks %s%s: no consensus (%u usable of %d samples, need %u); hop %ums/%lup; step %lu slew %lu; hold %lum",
               _prefs.clock_converge ? "on" : "off", clockIsUnset() ? " UNSET" : "",
               cc.n_seen, _obs.numClockSamples(), min_src, cc.hop_delay_ms,
               (unsigned long)_obs.hopDelayPairs(), (unsigned long)_clock_steps,
               (unsigned long)_clock_slews, (unsigned long)hold_m);
    }
  } else if (memcmp(what, "peers ", 6) == 0) {
    // One peer as JSON, so a host tool can page the whole table out: the human
    // summary below cannot show more than a handful inside a 160-byte reply,
    // and the table holds MAX_PEERS. Fields are terse for the same reason, and
    // anything unknown is omitted rather than sent as a zero.
    //   i/n index and total, h hash (hex, w bytes wide), d direct receptions,
    //   r relays seen, u times it relayed US, s mean SNR of direct sightings,
    //   m min hops (1 = ZERO-HOP, i.e. we hear it directly),
    //   a/da secs since any/direct sighting, sk clock skew, p pubkey, nm name
    int idx = atoi(&what[6]);
    int total = _obs.numPeers();
    const MeshObserver::PeerEntry* e = _obs.peer(idx);
    if (e == NULL) {
      snprintf(reply, reply_size, "{\"i\":%d,\"n\":%d}", idx, total);
      return;
    }
    char hash[8] = {0};
    for (int b = 0; b < e->width && b < 3; b++) sprintf(&hash[b*2], "%02x", e->hash[b]);
    unsigned long now = millis();
    int o = snprintf(reply, reply_size,
             "{\"i\":%d,\"n\":%d,\"h\":\"%s\",\"w\":%d,\"d\":%lu,\"r\":%lu,\"u\":%lu,\"m\":%d,\"a\":%lu",
             idx, total, hash, (int)e->width, (unsigned long)e->direct_rx,
             (unsigned long)e->relays, (unsigned long)e->heard_us, (int)e->min_hops,
             (unsigned long)((now - e->last_ms) / 1000));
    if (e->snr_n > 0 && o + 16 < (int)reply_size) {
      o += snprintf(&reply[o], reply_size - o, ",\"s\":%.1f,\"da\":%lu",
                    (float)e->snr4_sum / (4.0f * e->snr_n),
                    (unsigned long)((now - e->last_direct_ms) / 1000));
    }
    if (e->clock_n > 0 && o + 14 < (int)reply_size) {
      o += snprintf(&reply[o], reply_size - o, ",\"sk\":%ld", (long)e->clock_delta_s);
    }
    bool has_pub = false;
    for (int b = 0; b < 6; b++) if (e->pub[b]) has_pub = true;
    if (has_pub && o + 24 < (int)reply_size) {
      o += snprintf(&reply[o], reply_size - o, ",\"p\":\"%02x%02x%02x%02x%02x%02x\"",
                    e->pub[0], e->pub[1], e->pub[2], e->pub[3], e->pub[4], e->pub[5]);
    }
    if (e->name[0] && o + (int)strlen(e->name) + 10 < (int)reply_size) {
      o += snprintf(&reply[o], reply_size - o, ",\"nm\":\"%s\"", e->name);
    }
    snprintf(&reply[o], reply_size - o, "}");
  } else {   // peers
    // Churn matters as much as the count: a table sitting at its limit reads
    // the same whether it is calmly tracking the neighbourhood or thrashing.
    // Evictions say it is turning over; refusals say it is wedged full of live
    // peers and losing sightings. Widths say how much of it to trust at all.
    o += snprintf(reply, reply_size,
                  "%d/%d peers (%dx1B collision-prone), %d confirmed hearing us; evicted %lu, refused %lu:",
                  _obs.numPeers(), MeshObserver::MAX_PEERS, _obs.widthCount(1),
                  _obs.confirmedPeerCount(),
                  (unsigned long)_obs.evictions(), (unsigned long)_obs.refusedInserts());
    // nearest and most-heard first: those are the ones that describe our links
    for (int pass = 1; pass <= 2 && o + 24 < reply_size; pass++) {
      for (int i = 0; i < _obs.numPeers() && o + 24 < reply_size; i++) {
        const MeshObserver::PeerEntry* p = _obs.peer(i);
        if (p == nullptr || p->min_hops != pass || p->direct_rx == 0) continue;
        int snr4 = p->snr_n ? (int)(p->snr4_sum / (int32_t)p->snr_n) : 0;
        o += snprintf(reply + o, reply_size - o, " %02x%02x/%dh/%lurx/%+d",
                      p->hash[0], p->width > 1 ? p->hash[1] : 0, p->min_hops,
                      (unsigned long)p->direct_rx, snr4 / 4);
      }
    }
  }
}
#endif

void MyMesh::formatBridgeReply(char *reply, const char* what) {
  const size_t reply_size = 160;
  if (memcmp(what, "links", 5) == 0) {
    /* Connection-oriented peer links. "up" means the link layer is
       acknowledging and retrying for us; anything else means this peer is
       still being served by broadcast, with its measured loss. */
    int o = snprintf(reply, reply_size, "%d link(s) up:", (int)bridge.numLinks());
    for (uint8_t i = 0; i < BleLink::MAX_LINKS && o + 48 < (int)reply_size; i++) {
      ble_gap_addr_t a; bool up; int8_t rssi; uint32_t sent, recv, drops;
      uint32_t rx_age = 0xFFFFFFFF, queued = 0;
      if (!bridge.getLinkInfo(i, a, up, rssi, sent, recv, drops, &rx_age, &queued)) continue;
      /* rx age matters more than the counters: totals cannot tell a link that
         is carrying traffic from one that is nominally up and has been silent
         for an hour. Heartbeats put a frame on every link each interval, so
         anything past ~15s is already suspect. */
      char age[12];
      if (rx_age == 0xFFFFFFFF) strcpy(age, "never");
      else snprintf(age, sizeof(age), "%lus", (unsigned long)rx_age);
      o += snprintf(&reply[o], reply_size - o, " %02X%02X%02X/%s/tx%lu/rx%lu/drop%lu/q%lu/rx@%s",
                    a.addr[5], a.addr[4], a.addr[3], up ? "up" : "dialling",
                    (unsigned long)sent, (unsigned long)recv, (unsigned long)drops,
                    (unsigned long)queued, age);
    }
    if (o <= 14) snprintf(reply, reply_size, "no peer links (broadcast only)");
    return;
  }

  if (memcmp(what, "peers", 5) == 0) {
    // Who we are actually bridging with, and how good the link is. RSSI here is
    // the BLE link to that node, nothing to do with LoRa.
    uint8_t n = bridge.numPeers();
    int o = snprintf(reply, reply_size, "%d bridge peer(s):", (int)n);
    for (uint8_t i = 0; i < n && o + 30 < (int)reply_size; i++) {
      uint8_t addr[6];
      int8_t rssi;
      uint32_t age_ms, frames, copies, lost;
      int32_t skew_s;
      if (!bridge.getPeer(i, addr, rssi, age_ms, frames, skew_s, copies, lost)) break;
      /* loss  = datagrams of theirs we never saw a single copy of, from gaps in
                their sequence. This is the number that says whether the link is
                working.
         x     = copies actually received per datagram delivered. Against
                bridge.adv_rep it says whether the repeats are earning their
                airtime: x near adv_rep means the redundancy is wasted, x near
                1.0 means it is the only reason anything arrives.
         Fixed point throughout -- printf on this platform has no float. */
      uint32_t denom = frames + lost;
      unsigned long loss_x10 = denom ? (unsigned long)((uint64_t)lost * 1000 / denom) : 0;
      unsigned long cps_x100 = frames ? (unsigned long)((uint64_t)copies * 100 / frames) : 0;
      // BLE addresses are little-endian on the wire; the high 3 bytes are what
      // identifies a device at a glance.
      o += snprintf(&reply[o], reply_size - o,
                    " %02X%02X%02X/%ddB/%lupkt/%lus/skew%+lds/loss%lu.%lu%%/x%lu.%02lu",
                    addr[5], addr[4], addr[3], (int)rssi,
                    (unsigned long)frames, (unsigned long)(age_ms / 1000), (long)skew_s,
                    loss_x10 / 10, loss_x10 % 10, cps_x100 / 100, cps_x100 % 100);
    }
    if (n == 0) snprintf(reply, reply_size, "no bridge peers heard yet");
    return;
  }

  if (memcmp(what, "power", 5) == 0) {
    /* Charge/discharge inferred from voltage alone -- this hardware has no
       current sensing. Rates are NET (charger minus our own draw while
       charging). q is confidence: 2 good, 1 fair, 0 poor -- poor means we are
       on the flat middle of the lithium curve where voltage barely moves with
       charge, and the estimate should not be leaned on. */
    int32_t ma = _batt.milliAmps();
    int32_t hrs = _batt.hoursRemaining(PWRMGT_VOLTAGE_BOOTLOCK);
    char eta[24];
    if (hrs < 0)       strcpy(eta, "eta unknown");
    else if (ma > 0)   snprintf(eta, sizeof(eta), "full in %ldh", (long)hrs);
    else               snprintf(eta, sizeof(eta), "cutoff in %ldh", (long)hrs);
    snprintf(reply, reply_size,
             "power: %umV %u%% %+ldmA %+ldmW (%+ldmV/hr); %s; q%u n%u",
             (unsigned)_batt.latestMv(), (unsigned)_batt.percent(),
             (long)ma, (long)_batt.milliWatts(), (long)_batt.mvPerHour(),
             eta, (unsigned)_batt.sampleQuality(), (unsigned)_batt.numSamples());
    return;
  }

  if (memcmp(what, "cpu reset", 9) == 0) {
    /* max-gap is a high-water mark that nothing otherwise clears, so a single
       transient poisons it for the rest of the uptime and the number stops
       answering "is the loop healthy NOW". Both repeaters sat at ~2s purely
       because a CLI "set" wrote prefs -- a flash write blocks for about 1.6s
       while the SoftDevice arbitrates -- which says nothing about steady-state
       behaviour. Being able to zero it is what makes the measurement usable. */
    _loop_gap_max_ms = 0;
    _loop_last_ms = 0;
    strcpy(reply, "OK - loop stats reset");
    return;
  }

  if (memcmp(what, "cpu", 3) == 0) {
    /* What advert ingestion costs. The link layer decodes every advert on air
       whether or not the whitelist lets it through, so filtering moves this
       number and NOT radio current -- the battery saving is the CPU share
       only. rssi is the mean over reports we were given, which is the closest
       thing to an ambient reading available: the SoftDevice will not sample
       RSSI outside a connection. */
    /* Recoveries and advert failures live here rather than in "bridge" because
       they are health, not throughput: a node that has silently restarted its
       receive path three times is telling you something the packet counters
       cannot. */
    uint32_t us = bridge.reportCpuUs(), n = bridge.reportCount();
    uint32_t up_s = (uint32_t)(uptime_millis / 1000);
    snprintf(reply, reply_size,
             "ble ingest: %lu reports, %lu ms cpu (%lu us/report), %lu.%02lu%% of %lus uptime; "
             "mean rssi %ddB; loop %lu/s max-gap %lums; silence %lums; recoveries %lu; advfail %lu; "
             "lora idle %lus reinits %lu",
             (unsigned long)n, (unsigned long)(us / 1000),
             (unsigned long)(n ? us / n : 0),
             (unsigned long)(up_s ? (us / 10000) / up_s : 0),
             (unsigned long)(up_s ? ((us / 100) / up_s) % 100 : 0),
             (unsigned long)up_s, (int)bridge.meanReportRssi(),
             (unsigned long)_loop_rate, (unsigned long)_loop_gap_max_ms,
             (unsigned long)bridge.silenceMs(),
             (unsigned long)bridge.numRecoveries(), (unsigned long)bridge.numAdvFailures(),
             (unsigned long)(_lora_activity_ms ? (millis() - _lora_activity_ms) / 1000 : 0),
             (unsigned long)_lora_reinits);
    return;
  }

  // seen = ok + dup + bad + other, so the split says WHY frames were not used.
  // dup is expected and healthy (each datagram is deliberately broadcast over
  // several advertising events); other is ambient traffic from anyone else
  // using the shared 0xFFFF development company ID.
  // The secret is not optional -- every frame is tagged and every frame is
  // checked -- but the DEFAULT is published in this source file, so a node still
  // carrying it will accept anything anyone in radio range cares to inject.
  // Say so rather than let a green-looking line imply otherwise.
  const bool default_secret = (strcmp(_prefs.bridge_secret, "LVSITANOS") == 0);
  snprintf(reply, reply_size,
           "ble bridge %s%s: tx %lu drop %lu | rx seen %lu ok %lu hb %lu dup %lu bad %lu/L%lu other %lu | peers %d",
           bridge.isTransportUp() ? "up" : (bridge.isRunning() ? "starting" : "off"),
           default_secret ? " [DEFAULT SECRET - anyone can inject]" : "",
           (unsigned long)bridge.numSent(), (unsigned long)bridge.numTxDropped(),
           (unsigned long)bridge.numSeen(), (unsigned long)bridge.numRxOk(),
           (unsigned long)bridge.numHeartbeatsRx(),
           (unsigned long)bridge.numDup(), (unsigned long)bridge.numBadTagBcast(),
           (unsigned long)bridge.numBadTagLink(),
           (unsigned long)bridge.numForeign(), (int)bridge.numPeers());
}
#endif

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

#if WITH_BLE_CLI
void MyMesh::startBLE(SerialBLEInterface& ble, const char* name_prefix, char* name) {
  _ble = &ble;
  // Six digits, never zero-padded away, and never the shipped 123456.
  _ble_pin = 100000 + (uint32_t)getRNG()->nextInt(0, 900000);
  ble.begin(name_prefix, name, _ble_pin);
  ble.enable();
  Serial.printf("[ble] pairing PIN for this boot: %06lu\n", (unsigned long)_ble_pin);
}

/* The pairing PIN is regenerated every boot and upstream prints it only to USB
   serial. On a repeater sited without a cable that is unreadable, so a host
   whose bond went stale has no way back in -- and since SerialBLEInterface is
   also what carries DFU, no way to reflash either. Report it over the CLI,
   which a still-paired host or a mesh admin can reach. */
void MyMesh::formatBleReply(char *reply) {
  /* Report the slots the stack ACTUALLY came up with, not the ones asked for.
     Role counts drive the SoftDevice's RAM requirement and a request that does
     not fit silently degrades to the single-peripheral fallback -- which looks
     identical from outside until something tries to open a second link. */
  snprintf(reply, 160,
           "ble on, PIN %06lu, connected=%s; slots %up/%uc mtu %u q%u; beacon %lu err 0x%lX",
           (unsigned long)_ble_pin,
           (_ble != nullptr && _ble->isConnected()) ? "yes" : "no",
           (unsigned)BleStack::periphSlots(), (unsigned)BleStack::centralSlots(),
           /* What the RAM ladder actually settled for. mtu 23 means every link
              frame is fragmented; q1 means the fragments cannot be pipelined,
              which together discarded a third of link traffic. */
           (unsigned)BleStack::mtu(), (unsigned)BleStack::txQueueSize(),
           /* Presence beacon: how many have gone out, and the last SoftDevice
              error if any. A silent zero here means it never ran. */
           (unsigned long)bridge.numPresenceAdverts(),
           (unsigned long)bridge.presenceError());
}

void MyMesh::bleLoop() {
  if (_ble == nullptr || !_ble->isConnected()) return;
  uint8_t frame[MAX_FRAME_SIZE + 1];
  size_t n = _ble->checkRecvFrame(frame);
  if (n == 0 || _ble->isWriteBusy()) return;
  if (n > MAX_FRAME_SIZE) n = MAX_FRAME_SIZE;
  frame[n] = 0;                       // the CLI wants a C string

  // A non-zero timestamp deliberately withholds the commands CommonCLI gates to
  // local serial only -- erase, log, set freq, set prv.key. BLE reaches tens of
  // metres, so those stay behind physical USB access even though pairing is
  // encrypted and MITM-protected.
  char reply[MAX_FRAME_SIZE];
  reply[0] = 0;
  handleCommand(++_ble_seq, (char *) frame, reply);
  if (reply[0]) _ble->writeFrame((const uint8_t *) reply, strlen(reply));
}
#endif

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

/* See the notes in MyMesh.h. Staged: notice a long silence, prove the radio can
   still transmit, reinitialise it if it cannot, and reboot only if that fails
   too. Every stage is paced and every comparison signed -- an unpaced retry and
   an unsigned elapsed-time test have each already cost this project a node. */
void MyMesh::loraWatchdog() {
  unsigned long now = millis();
  if (_lora_next_check_ms != 0 && (long)(now - _lora_next_check_ms) < 0) return;
  _lora_next_check_ms = now + LORA_CHECK_EVERY_MS;

  unsigned long air = getTotalAirTime() + getReceiveAirTime();

  if (air != _lora_last_air) {              // radio demonstrably working
    _lora_last_air = air;
    _lora_activity_ms = now;
    _lora_wd_state = LORA_WD_IDLE;
    return;
  }
  if (_lora_activity_ms == 0) { _lora_activity_ms = now; return; }

  switch (_lora_wd_state) {
    case LORA_WD_IDLE:
      if ((long)(now - _lora_activity_ms) < (long)LORA_IDLE_MS) return;
      /* Make our own traffic rather than wait for someone else's: on a quiet
         band nobody may ever transmit, and silence would be misread as death. */
      _lora_test_air = air;
      _lora_test_started_ms = now;
      _lora_wd_state = LORA_WD_TESTING;
      sendSelfAdvertisement(500, false);     // zero-hop, cheap, no flood
      MESH_DEBUG_PRINTLN("LoRa watchdog: silent %lus, probing radio",
                         (unsigned long)((now - _lora_activity_ms) / 1000));
      return;

    case LORA_WD_TESTING:
      if ((long)(now - _lora_test_started_ms) < (long)LORA_SELFTEST_GRACE_MS) return;
      if (air != _lora_test_air) {           // it transmitted: radio is alive
        _lora_last_air = air;
        _lora_activity_ms = now;
        _lora_wd_state = LORA_WD_IDLE;
        return;
      }
      /* Asked to transmit and no airtime resulted. Reinitialise, and restore
         every parameter begin() sets -- a bare radio_init() would leave the
         node on the driver's default frequency, silently off-band, which is
         worse than the fault being repaired. */
      _lora_reinits++;
      MESH_DEBUG_PRINTLN("LoRa watchdog: no airtime after probe, reinitialising");
      radio_init();
      radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
      radio_driver.setTxPower(_prefs.tx_power_dbm);
      radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
      board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);
      board.setLoRaFemPaGainEnabled(_prefs.radio_fem_txgain);
      _lora_test_started_ms = now;
      _lora_test_air = getTotalAirTime() + getReceiveAirTime();
      _lora_wd_state = LORA_WD_REINITED;
      sendSelfAdvertisement(500, false);
      return;

    case LORA_WD_REINITED:
      if ((long)(now - _lora_test_started_ms) < (long)LORA_SELFTEST_GRACE_MS) return;
      if (air != _lora_test_air) {           // reinit worked
        _lora_last_air = air;
        _lora_activity_ms = now;
        _lora_wd_state = LORA_WD_IDLE;
        return;
      }
      /* Reinitialised and still cannot transmit. Nothing else here can help,
         and a repeater that cannot use its radio is doing nothing at all. */
      MESH_DEBUG_PRINTLN("LoRa watchdog: dead after reinit, rebooting");
      _cli.savePrefs(_fs);                   // deferred writes would be lost
      board.reboot();
      return;
  }
}

void MyMesh::loop() {
  /* Sampled once a second. This task is TASK_PRIO_LOW and every BLE advert
     report preempts it, so the rate is a whole-system proxy for what the radio
     side is taking -- one increment, no core patch, no dedicated timer. */
  {
    unsigned long lt = millis();
    if (_loop_last_ms != 0) {
      unsigned long gap = lt - _loop_last_ms;
      if (gap > _loop_gap_max_ms) _loop_gap_max_ms = gap;
    }
    _loop_last_ms = lt;
    _loop_iters++;
    /* Gate the ADC read on due(): the loop runs ~16k times a second and an
       ADC conversion is not free. One sample a minute is all this needs. */
    if (_batt.due()) _batt.update(board.getBattMilliVolts());
#ifdef WITH_BLE_BRIDGE
    /* Keep the presence beacon's payload current. It is what a scanner sees
       when this node has no free peripheral slot and cannot advertise
       connectably -- name and battery are enough to triage it without ever
       opening a connection. */
    bridge.setPresenceInfo(_prefs.node_name, _batt.latestMv());
#endif
#ifdef LOOP_WATCHDOG_MS
    LoopWatchdog::feed();
    /* First pass proves the loop is actually running, which is the only point
       at which the tight runtime limit is safe to impose. Until now the boot
       limit from main.cpp has been covering setup(). */
    if (!_wdog_tightened) {
      _wdog_tightened = true;
      LoopWatchdog::setLimit(LOOP_WATCHDOG_MS);
    }
#endif
    if (lt - _loop_rate_ms >= 1000) {
      _loop_rate = _loop_iters;
      _loop_iters = 0;
      _loop_rate_ms = lt;
    }
  }

#ifdef WITH_BRIDGE
  bridge.loop();
#endif

#if WITH_BLE_CLI
  bleLoop();
#endif

  mesh::Mesh::loop();

#if WITH_MESH_OBSERVER
  maybeConvergeClock();
#endif

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  /* Settled long enough that more settings are unlikely to follow. */
  loraWatchdog();

  if (_prefs_dirty_ms != 0 && (long)(millis() - _prefs_dirty_ms) >= (long)PREFS_SETTLE_MS) {
    _prefs_dirty_ms = 0;
    _cli.savePrefs(_fs);
  }

  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BLE_BRIDGE)
  // Unlike the WiFi bridges, this one can sleep. nRF52 sleep is event-driven
  // (sd_app_evt_wait), and SoftDevice radio events wake the CPU, so packets
  // still arrive over BLE while asleep. Only transmission needs the loop to run
  // on time -- the advertising-set arbiter works to millis() deadlines, and a
  // queued datagram would otherwise wait for some unrelated interrupt.
  if (bridge.hasPendingTx()) return true;
#elif defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundTotal() > 0;
}
