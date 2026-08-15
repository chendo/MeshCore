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

bool BLEBridge::peerAllows(const uint8_t addr[6], uint32_t timestamp, const uint8_t *tag) const {
  const PeerStamp *slot = nullptr;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    if (_peers[i].in_use && memcmp(_peers[i].addr, addr, 6) == 0) { slot = &_peers[i]; break; }
  }

  /* First frame from this sender: allow, exactly as BaseChatMesh does for an
     advert from an unknown contact. */
  if (slot == nullptr) return true;

  /* Silence the deliberate repeats. BleBroadcast sends each datagram over
     max_adv_evts advertising events so a duty-cycled scanner cannot miss it
     entirely, which means the same frame usually arrives two or three times.
     The tag is an HMAC over the whole frame, so it identifies one uniquely.
     Caught here, a repeat costs nothing; left to _seen_packets it would first
     consume a packet buffer and a hash. */
  if (memcmp(slot->last_tag, tag, TAG_SIZE) == 0) return false;

  /* A sender unheard for a long while gets a fresh start -- the escape hatch
     for a node whose clock restarted lower across a reboot. */
  if (millis() - slot->last_seen > PEER_STALE_MS) return true;

  /* Strictly OLDER is a replay. Equal is not: the timestamp is RTC seconds, and
     a busy repeater bridges several distinct packets inside one second, so
     requiring strictly-increasing would drop every one after the first. */
  if (timestamp < slot->last_timestamp) return false;

  return true;
}

void BLEBridge::peerAccept(const uint8_t addr[6], uint32_t timestamp, const uint8_t *tag) {
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
  memcpy(slot->last_tag, tag, TAG_SIZE);
}

void BLEBridge::onFrameRecv(const uint8_t *payload, uint8_t len, const uint8_t addr[6], int8_t rssi) {
  if (len < HEADER_SIZE + TAG_SIZE + 1) return;         // no room for a packet
  if (payload[0] != FRAME_VERSION) return;              // not ours, or newer

  const size_t signed_len = len - TAG_SIZE;
  const size_t packet_len = signed_len - HEADER_SIZE;
  if (packet_len > MAX_PAYLOAD_SIZE) return;

  uint32_t timestamp;
  memcpy(&timestamp, &payload[VERSION_SIZE], TIMESTAMP_SIZE);

  const uint8_t *tag = &payload[signed_len];

  /* Cheap gate first: a stale frame or a repeat is rejected without doing any
     crypto, and a forged one cannot get past the tag anyway. Note this only
     READS peer state -- see peerAllows(). */
  if (!peerAllows(addr, timestamp, tag)) {
    _num_replayed++;
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
  peerAccept(addr, timestamp, tag);

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
