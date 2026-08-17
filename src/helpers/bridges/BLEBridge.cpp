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

void BLEBridge::rx_cb(const uint8_t *payload, uint8_t len, const uint8_t addr[6],
                      uint8_t addr_type, int8_t rssi) {
  if (_instance) {
    _instance->onFrameRecv(payload, len, addr, addr_type, rssi);
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
    /* Links first. Leaving them connected would hold a peripheral slot on the
       peer, which stops IT advertising -- see BleLink::end(). */
    _link.end();
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
      ble_gap_addr_t self;
      if (sd_ble_gap_addr_get(&self) == NRF_SUCCESS) _link.begin(link_rx_cb, self);
      BRIDGE_DEBUG_PRINTLN("BLE: broadcast up\n");
    } else {
      _next_start_attempt = now + START_RETRY_MS;
      BRIDGE_DEBUG_PRINTLN("BLE: broadcast failed to start, retrying later\n");
      return;
    }
  }

  // Cheap, and lets both knobs be retuned over the CLI without a reboot.
  _bcast.setAdvRepeat(_prefs->bridge_adv_repeat);
  _bcast.setTxHoldMs(_prefs->bridge_ble_hold);
  _bcast.setScanDuty(_prefs->bridge_scan_duty);
  _bcast.setScanFilter(_prefs->bridge_scan_filter != 0);
  _link.loop();
  /* Connecting stops the scanner; re-arm whenever the link topology moved. */
  if (_link.takeTopologyChanged()) _bcast.requestRescan();

  /* The transport can take itself down when its receive path stops answering.
     Rebuild it rather than leaving the bridge nominally up and permanently
     deaf -- begin() is idempotent and re-arms scanning from scratch. */
  if (!_bcast.isRunning()) {
    _transport_up = false;
    _next_start_attempt = millis() + START_RETRY_MS;
    BRIDGE_DEBUG_PRINTLN("BLE: transport went down, will restart\n");
  }

  /* Only peers that have passed the HMAC get whitelisted, so an attacker
     cannot talk their way into our scan filter -- and cannot talk everyone
     else out of it either. */
  if (_peers_gen != _wl_pushed_gen) {
    _wl_pushed_gen = _peers_gen;
    ble_gap_addr_t wl[MAX_PEERS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_PEERS && n < MAX_PEERS; i++) {
      if (!_peers[i].in_use) continue;
      memset(&wl[n], 0, sizeof(wl[n]));
      wl[n].addr_type = _peers[i].addr_type;
      memcpy(wl[n].addr, _peers[i].addr, 6);
      n++;
    }
    _bcast.setWhitelist(wl, n);
  }

  /* Timed rather than "when idle": a node that bridges occasionally would
     otherwise still go long enough between datagrams for a peer to age out. */
  if (_transport_up) {
    unsigned long now = millis();
    if (_next_hb_ms == 0) _next_hb_ms = now + HEARTBEAT_MS;
    else if ((long)(now - _next_hb_ms) >= 0) {
      _next_hb_ms = now + HEARTBEAT_MS;
      sendHeartbeat();
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

void BLEBridge::link_rx_cb(const uint8_t* data, uint16_t len, uint8_t link_idx) {
  if (_instance) _instance->onLinkFrame(data, len, link_idx);
}

/* Same validation as the broadcast path minus the duplicate check: a link
   delivers each frame exactly once, so a repeat there would be an anomaly
   rather than the deliberate redundancy broadcasting depends on. */
void BLEBridge::onLinkFrame(const uint8_t* data, uint16_t len, uint8_t link_idx) {
  const bool is_hb = (data[0] == FRAME_HEARTBEAT);
  if ((data[0] != FRAME_VERSION && !is_hb) ||
      len < HEADER_SIZE + TAG_SIZE + (is_hb ? 0 : 1)) {
    _num_foreign++;
    return;
  }
  const size_t signed_len = len - TAG_SIZE;
  uint8_t expected[TAG_SIZE];
  computeTag(data, signed_len, expected);
  if (memcmp(expected, &data[signed_len], TAG_SIZE) != 0) {
    /* Still checked. BLE authenticates the LINK; the group tag is what says
       this peer belongs to our bridge rather than merely speaking the protocol.

       Counted separately from the broadcast path. Arriving here means bytes
       crossed an acknowledged connection and STILL failed the tag -- the
       controller does not deliver corrupt payloads, so this is our own framing
       (reassembly in BleLink), not interference. Any sustained count is a bug. */
    _num_bad_tag_link++;
    return;
  }

  /* Tag passed, so this peer is genuinely one of ours -- the only evidence of
     group membership there is. Told to BleLink so it can drop a link that never
     produces it instead of holding the slot open forever. Heartbeats count:
     they are tagged too, and they are what a quiet peer sends. */
  _link.markAuthed(link_idx);

  if (is_hb) { _num_hb_rx++; return; }

  _num_rx_ok++;


  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) return;
  if (pkt->readFrom(&data[HEADER_SIZE], (uint8_t)(signed_len - HEADER_SIZE))) {
    onPacketReceived(pkt);
  } else {
    _mgr->free(pkt);
  }
  (void)link_idx;
}

/* Prefer an established link and fall back to broadcasting. A link is reliable
   and a broadcast is not, so where both are possible the link wins -- but a
   peer we have no connection to is still only reachable the old way, which is
   why the broadcast transport stays rather than being replaced. */
bool BLEBridge::dispatch(const uint8_t* frame, uint16_t len) {
  if (_link.send(frame, len) > 0) return true;
  return _bcast.send(frame, (uint8_t)len);
}

void BLEBridge::sendHeartbeat() {
  uint8_t *frame = _tx_frame;
  frame[0] = FRAME_HEARTBEAT;

  /* Shares the datagram counter with real traffic, so sequence gaps measure
     the link continuously instead of only while something is being bridged. */
  uint16_t seq = _tx_seq++;
  memcpy(&frame[VERSION_SIZE], &seq, SEQ_SIZE);
  uint32_t timestamp = _rtc->getCurrentTime();
  memcpy(&frame[VERSION_SIZE + SEQ_SIZE], &timestamp, TIMESTAMP_SIZE);

  computeTag(frame, HEADER_SIZE, &frame[HEADER_SIZE]);
  const uint16_t hb_len = (uint16_t)(HEADER_SIZE + TAG_SIZE);

  /* Broadcast, so a peer we have no connection to can still discover us. */
  _bcast.send(frame, (uint8_t)hb_len);

  /* And down every established link, as keepalive. Without this a link with no
     packets to carry looks identical to a dead one: the supervision timeout
     only notices a radio that has gone away, not a peer that has stopped
     talking, so an idle check needs a guaranteed floor of traffic to measure
     against. At one heartbeat per 15s against a 60s idle limit there are four
     chances to miss before a link is judged dead. */
  _link.sendKeepalive(frame, hb_len);
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

  /* Counts datagrams, not transmissions: all adv_rep copies of one datagram
     carry the same number, so a receiver counting distinct sequence values
     measures delivery while counting arrivals measures redundancy. */
  uint16_t seq = _tx_seq++;
  memcpy(&frame[VERSION_SIZE], &seq, SEQ_SIZE);

  /* Sender's clock. Little-endian to match the advert timestamp in the mesh
     protocol itself. Diagnostic only -- see the note in onFrameRecv. */
  uint32_t timestamp = _rtc->getCurrentTime();
  memcpy(&frame[VERSION_SIZE + SEQ_SIZE], &timestamp, TIMESTAMP_SIZE);
  memcpy(&frame[HEADER_SIZE], _staging, packet_len);

  /* Tag covers version, timestamp and packet -- everything before it -- so no
     field is malleable. Contiguous, so one HMAC pass does it. */
  const size_t signed_len = HEADER_SIZE + packet_len;
  computeTag(frame, signed_len, &frame[signed_len]);

  if (dispatch(frame, (uint16_t)(signed_len + TAG_SIZE))) {
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
                        uint32_t &frames, int32_t &skew_s,
                        uint32_t &copies, uint32_t &lost) const {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    const PeerStamp *p = &_peers[i];
    if (!p->in_use) continue;
    if (seen++ != idx) continue;
    memcpy(addr, p->addr, 6);
    rssi = p->last_rssi;
    age_ms = (uint32_t)(millis() - p->last_seen);
    frames = p->frames;
    copies = p->copies;
    lost = p->lost;
    /* Age the peer's reading forward before comparing. Their clock kept running
       after they stamped that frame, so comparing a timestamp captured age_ms
       ago against now charges the idle time to them as clock error -- a peer
       silent for five minutes reads as five minutes fast. */
    uint32_t ours = _rtc->getCurrentTime();
    skew_s = (int32_t)((p->last_timestamp + age_ms / 1000) - ours);
    return true;
  }
  return false;
}

void BLEBridge::peerCountCopy(const uint8_t addr[6]) {
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    if (_peers[i].in_use && memcmp(_peers[i].addr, addr, 6) == 0) {
      _peers[i].copies++;
      return;
    }
  }
}

void BLEBridge::peerAccept(const uint8_t addr[6], uint8_t addr_type, uint32_t timestamp,
                           const uint8_t *tag, int8_t rssi, uint16_t seq) {
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
    slot->addr_type = addr_type;
    slot->in_use = true;
    _peers_gen++;              // the whitelist needs rebuilding

    /* Only now, with the group tag verified, is this an address we are willing
       to dial. Feeding the link layer from raw scan results would let anyone in
       radio range choose who we connect to. */
    ble_gap_addr_t pa;
    memset(&pa, 0, sizeof(pa));
    pa.addr_type = addr_type;
    memcpy(pa.addr, addr, 6);
    _link.notePeer(pa);
  }

  /* Gap accounting, in uint16 arithmetic so the wrap at 65535 costs nothing.
     A gap of 1 means consecutive; anything larger is that many datagrams we
     never saw a single copy of. */
  if (!slot->seq_valid) {
    slot->seq_valid = true;                       // first frame: nothing to compare
  } else {
    uint16_t advance = (uint16_t)(seq - slot->last_seq);
    if (advance == 0) {
      // same datagram, different tag -- cannot happen via dedup, ignore
    } else if (advance > SEQ_RESET_GAP) {
      slot->seq_valid = true;                     // they restarted; do not blame the link
    } else {
      slot->lost += (uint32_t)(advance - 1);
    }
  }
  slot->last_seq = seq;

  slot->last_timestamp = timestamp;
  slot->last_seen = now;
  slot->last_rssi = rssi;
  slot->frames++;
  slot->copies++;
  memcpy(slot->last_tag, tag, TAG_SIZE);
}

void BLEBridge::onFrameRecv(const uint8_t *payload, uint8_t len, const uint8_t addr[6],
                            uint8_t addr_type, int8_t rssi) {
  /* Not our protocol: too short to be a frame, a version we do not speak, or
     impossibly large. 0xFFFF is the SIG's shared development company ID, so
     other people's beacons legitimately arrive here and must be counted, or the
     telemetry stops adding up. */
  const bool is_hb = (payload[0] == FRAME_HEARTBEAT);
  /* A heartbeat carries no packet, so it is one byte shorter than the minimum
     a data frame may be. */
  if ((payload[0] != FRAME_VERSION && !is_hb) ||
      len < HEADER_SIZE + TAG_SIZE + (is_hb ? 0 : 1)) {
    _num_foreign++;
    return;
  }

  const size_t signed_len = len - TAG_SIZE;
  const size_t packet_len = signed_len - HEADER_SIZE;
  if (packet_len > MAX_PAYLOAD_SIZE) {
    _num_foreign++;
    return;
  }

  uint16_t seq;
  memcpy(&seq, &payload[VERSION_SIZE], SEQ_SIZE);
  uint32_t timestamp;
  memcpy(&timestamp, &payload[VERSION_SIZE + SEQ_SIZE], TIMESTAMP_SIZE);

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
    /* A repeat of a frame this peer was already authenticated for, so counting
       it cannot be driven by an unauthenticated sender. This is the numerator
       of "how many of the adv_rep copies actually arrive". */
    peerCountCopy(addr);
    return;
  }

  uint8_t expected[TAG_SIZE];
  computeTag(payload, signed_len, expected);
  if (memcmp(expected, tag, TAG_SIZE) != 0) {
    /* Wrong group secret, or someone playing. This is the same role
       ESPNowBridge's checksum plays: it is what keeps neighbouring bridge
       groups from bleeding into each other. Expected background on an open
       band -- see numBadTagBcast(). */
    _num_bad_tag_bcast++;
    BRIDGE_DEBUG_PRINTLN("BLE: RX bad tag, len=%d rssi=%d\n", (int)len, (int)rssi);
    return;
  }

  /* Authenticated: only now is it safe to move this sender's high-water mark. */
  peerAccept(addr, addr_type, timestamp, tag, rssi, seq);

  if (is_hb) {
    /* Everything a heartbeat exists for has now happened: the peer is known,
       its address is learnable for a future connection, and the sequence gap
       accounting has advanced. There is no packet to hand upward. */
    _num_hb_rx++;
    BRIDGE_DEBUG_PRINTLN("BLE: heartbeat from peer, rssi=%d\n", (int)rssi);
    return;
  }

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
