#include "BLEBridge.h"

#ifdef WITH_BLE_BRIDGE

#include <Arduino.h>
#include <SHA256.h>
#if WITH_STATUS_LED
  #include "helpers/StatusLed.h"
#endif
#include <string.h>

BLEBridge *BLEBridge::_instance = nullptr;
BleBroadcast::event_chain_t BLEBridge::_chain = nullptr;
bool BLEBridge::_ble_ready = false;

void BLEBridge::setBleReady(BleBroadcast::event_chain_t chain) {
  _chain = chain;
  _ble_ready = true;
}

void BLEBridge::rx_cb(const uint8_t *payload, uint8_t len, const uint8_t addr[6], int8_t rssi) {
  if (_instance) {
    _instance->onFrameRecv(payload, len, addr, rssi);
  }
}

BLEBridge::BLEBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc) {
  _instance = this;
  memset(_peers, 0, sizeof(_peers));
  memset(_key, 0, sizeof(_key));
}

void BLEBridge::deriveKey() {
  /* bridge.secret is a short ASCII string, not key material. Hashing it gives
     the HMAC a full-width key and stops the secret's length from mattering.
     (ESPNowBridge XORs with the raw ASCII, which is the thing being fixed.) */
  const char *secret = _prefs->bridge_secret;
  mesh::Utils::sha256(_key, KEY_SIZE, (const uint8_t *)secret, strlen(secret));
}

void BLEBridge::begin() {
  BRIDGE_DEBUG_PRINTLN("BLE: initializing, max payload %d\n", (int)MAX_PAYLOAD_SIZE);

  deriveKey();
  memset(_peers, 0, sizeof(_peers));
  _next_start_attempt = 0;

  /* The transport is NOT started here. See setBleReady(): at this point in
     boot the BLE stack does not exist yet, and starting now would have our
     event callback overwritten moments later. loop() completes the job. */
  _initialized = true;
}

void BLEBridge::end() {
  BRIDGE_DEBUG_PRINTLN("BLE: stopping\n");

  if (_transport_up) {
    _bcast.end();
    _transport_up = false;
  }
  _initialized = false;
}

void BLEBridge::loop() {
  if (!_initialized) return;

  if (!_transport_up) {
    if (!_ble_ready) return;                       // BLE stack not up yet
    unsigned long now = millis();
    if (_next_start_attempt != 0 && (long)(now - _next_start_attempt) < 0) return;

    if (_bcast.begin(COMPANY_ID, rx_cb, _chain)) {
      _transport_up = true;
      BRIDGE_DEBUG_PRINTLN("BLE: broadcast up\n");
    } else {
      _next_start_attempt = now + START_RETRY_MS;
      BRIDGE_DEBUG_PRINTLN("BLE: broadcast failed to start, retrying later\n");
      return;
    }
  }

  _bcast.loop();
}

void BLEBridge::computeTag(const uint8_t *frame, size_t len, uint8_t tag[TAG_SIZE]) {
  SHA256 sha;
  sha.resetHMAC(_key, KEY_SIZE);
  sha.update(frame, len);
  sha.finalizeHMAC(_key, KEY_SIZE, tag, TAG_SIZE);
}

void BLEBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !_transport_up) return;

  if (!packet) {
    BRIDGE_DEBUG_PRINTLN("BLE: TX invalid packet pointer\n");
    return;
  }

  if (_seen_packets.wasSeen(packet)) return;
  _seen_packets.markSeen(packet);

  /* writeTo() documents its destination as MAX_MTU_SIZE and does not bound
     itself, so it gets a full-size buffer. Serialising straight into the frame
     would leave it only MAX_PAYLOAD_SIZE bytes of room and a large packet would
     run off the end. Both buffers are members: this is called from deep in the
     dispatcher's call stack via logTx. */
  uint8_t packet_len = packet->writeTo(_staging);
  if (packet_len > MAX_PAYLOAD_SIZE) {
    BRIDGE_DEBUG_PRINTLN("BLE: TX packet too large (len=%d, max=%d)\n", (int)packet_len,
                         (int)MAX_PAYLOAD_SIZE);
    return;
  }

  uint8_t *frame = _tx_frame;
  frame[0] = FRAME_VERSION;

  /* Sender's clock, for the receiver's replay gate. Little-endian to match the
     advert timestamp in the mesh protocol itself. */
  uint32_t timestamp = _rtc->getCurrentTime();
  memcpy(&frame[VERSION_SIZE], &timestamp, TIMESTAMP_SIZE);
  memcpy(&frame[HEADER_SIZE], _staging, packet_len);

  /* Tag covers version, timestamp and packet -- everything before it -- so no
     field is malleable. Contiguous, so one HMAC pass does it. */
  const size_t signed_len = HEADER_SIZE + packet_len;
  computeTag(frame, signed_len, &frame[signed_len]);

  if (_bcast.send(frame, (uint8_t)(signed_len + TAG_SIZE))) {
#if WITH_STATUS_LED
    StatusLed::bleTx();
#endif
    BRIDGE_DEBUG_PRINTLN("BLE: TX, len=%d\n", (int)packet_len);
  } else {
    BRIDGE_DEBUG_PRINTLN("BLE: TX failed\n");
  }
}

bool BLEBridge::isDuplicate(const uint8_t addr[6], const uint8_t *tag) const {
  /* The deliberate repeats. BleBroadcast sends each datagram over max_adv_evts
     advertising events so a duty-cycled scanner cannot miss it entirely, so the
     same frame normally arrives two or three times. The tag is an HMAC over the
     whole frame, so it identifies one uniquely. Caught here a repeat costs
     nothing; left to _seen_packets it would first consume a packet buffer and a
     hash. Counted separately from a stale frame so the telemetry can tell
     "working as designed" from "someone is replaying at us". */
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    if (_peers[i].in_use && memcmp(_peers[i].addr, addr, 6) == 0) {
      return memcmp(_peers[i].last_tag, tag, TAG_SIZE) == 0;
    }
  }
  return false;
}

uint8_t BLEBridge::numPeers() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].in_use) n++;
  return n;
}

bool BLEBridge::getPeer(uint8_t idx, uint8_t addr[6], int8_t &rssi, uint32_t &age_ms,
                        uint32_t &frames, int32_t &skew_s) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    if (!_peers[i].in_use) continue;
    if (n++ != idx) continue;
    memcpy(addr, _peers[i].addr, 6);
    rssi = _peers[i].last_rssi;
    age_ms = (uint32_t)(millis() - _peers[i].last_seen);
    frames = _peers[i].frames;
    skew_s = (int32_t)(_peers[i].last_timestamp - _rtc->getCurrentTime());
    return true;
  }
  return false;
}

void BLEBridge::peerAccept(const uint8_t addr[6], uint32_t timestamp, const uint8_t *tag,
                           int8_t rssi) {
  unsigned long now = millis();
  PeerStamp *slot = nullptr, *victim = nullptr;

  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    PeerStamp *p = &_peers[i];
    if (p->in_use && memcmp(p->addr, addr, 6) == 0) { slot = p; break; }
    if (!p->in_use) {
      if (victim == nullptr || victim->in_use) victim = p;
    } else if (victim == nullptr || (victim->in_use && p->last_seen < victim->last_seen)) {
      victim = p;
    }
  }

  if (slot == nullptr) {
    slot = victim;
    if (slot == nullptr) return;          // every slot fresh; nothing sane to evict
    memcpy(slot->addr, addr, 6);
    slot->in_use = true;
  }

  slot->last_timestamp = timestamp;
  slot->last_seen = now;
  slot->last_rssi = rssi;
  slot->frames++;
  memcpy(slot->last_tag, tag, TAG_SIZE);
}

void BLEBridge::onFrameRecv(const uint8_t *payload, uint8_t len, const uint8_t addr[6], int8_t rssi) {
  /* Not our protocol: too short to be a frame, a version we do not speak, or
     impossibly large. 0xFFFF is the SIG's shared development company ID, so
     other people's beacons legitimately arrive here and must be counted, or the
     telemetry stops adding up. */
  if (len < HEADER_SIZE + TAG_SIZE + 1 || payload[0] != FRAME_VERSION) {
    _num_foreign++;
    return;
  }

  const size_t signed_len = len - TAG_SIZE;
  const size_t packet_len = signed_len - HEADER_SIZE;
  if (packet_len > MAX_PAYLOAD_SIZE) {
    _num_foreign++;
    return;
  }

  uint32_t timestamp;
  memcpy(&timestamp, &payload[VERSION_SIZE], TIMESTAMP_SIZE);

  const uint8_t *tag = &payload[signed_len];

  /* Cheap gate first: a repeat is discarded without doing any crypto, and a
     forgery cannot get past the tag anyway. Read-only, so nothing an attacker
     sends can influence peer state before it authenticates.
     
     There is deliberately no freshness check on the timestamp. Rejecting frames
     older than the last one from a sender bought almost nothing -- a replay is
     already caught by _seen_packets here, by the mesh's own dedup table, and by
     MeshCore's login/admin/advert timestamp checks, none of which the bridge can
     weaken -- while costing real availability: these boards have no hardware
     RTC, so any reboot moves a node's clock backwards and got it ignored for
     minutes. The timestamp is still carried and still covered by the HMAC; it is
     now purely a diagnostic (see getPeer's skew_s). */
  if (isDuplicate(addr, tag)) {
    _num_dup++;
    return;
  }

  uint8_t expected[TAG_SIZE];
  computeTag(payload, signed_len, expected);
  if (memcmp(expected, tag, TAG_SIZE) != 0) {
    /* Wrong group secret, or someone playing. This is the same role
       ESPNowBridge's checksum plays: it is what keeps neighbouring bridge
       groups from bleeding into each other. */
    _num_bad_tag++;
    BRIDGE_DEBUG_PRINTLN("BLE: RX bad tag, len=%d rssi=%d\n", (int)len, (int)rssi);
    return;
  }

  /* Authenticated: only now is it safe to move this sender's high-water mark. */
  peerAccept(addr, timestamp, tag, rssi);
  _num_rx_ok++;

#if WITH_STATUS_LED
  // A bridged packet never reaches logRxRaw -- it is queued straight inbound --
  // so the LoRa hooks never see it. Blue is the bridge's own colour anyway.
  StatusLed::bleRx();
#endif
  BRIDGE_DEBUG_PRINTLN("BLE: RX, payload_len=%d rssi=%d\n", (int)packet_len, (int)rssi);

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) return;

  if (pkt->readFrom(&payload[HEADER_SIZE], (uint8_t)packet_len)) {
    onPacketReceived(pkt);
  } else {
    _mgr->free(pkt);
  }
}

void BLEBridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}

#endif
