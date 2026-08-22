#include "BLEBridge.h"

#include <Arduino.h>
#include <string.h>
#include <target.h>
#if WITH_STATUS_LED
  #include "helpers/StatusLed.h"
#endif

#include "helpers/nrf52/BleStack.h"

BLEBridge *BLEBridge::_instance = nullptr;

BLEBridge::BLEBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc) {
  _instance = this;
  memset(_stamps, 0, sizeof(_stamps));
}

void BLEBridge::begin() {
  BRIDGE_DEBUG_PRINTLN("BLE: initializing, max payload %d\n",
                       (int)BleBridgeFrame::MAX_PAYLOAD_SIZE);

  /* Derive the key now, so `set bridge.secret` plus restartBridge() picks up
     the new value. The group marker follows from the same key. */
  _codec.setSecret(_prefs->bridge_secret);
  memset(_stamps, 0, sizeof(_stamps));
  _next_start_attempt = 0;

  /* The transport does NOT start here. MyMesh::begin() runs this deep inside
     setup(), and anything else on the build that wants BLE has not had its turn
     yet. loop() completes the job on the first pass after setup() returns. */
  _initialized = true;
}

void BLEBridge::end() {
  BRIDGE_DEBUG_PRINTLN("BLE: stopping\n");

  if (_transport_up) {
    /* Links first. A link left connected holds a peripheral slot on the peer,
       which stops IT from advertising. See BleLink::end(). */
    _link.end();
    _disc.end();
    _transport_up = false;
  }
  _initialized = false;
}

void BLEBridge::loop() {
  if (!_initialized) return;

  if (!_transport_up) {
    unsigned long now = millis();
    if (_next_start_attempt != 0 && (long)(now - _next_start_attempt) < 0) return;

    char ble_name[40];
    snprintf(ble_name, sizeof(ble_name), "MeshCore-%s", _prefs->node_name);
    if (!BleStack::ensure(ble_name, BLE_PRPH_SLOTS, BLE_CENTRAL_SLOTS)) {
      _next_start_attempt = now + START_RETRY_MS;
      BRIDGE_DEBUG_PRINTLN("BLE: stack failed to start, retrying later\n");
      return;
    }

    ble_gap_addr_t self;
    if (sd_ble_gap_addr_get(&self) != NRF_SUCCESS) {
      _next_start_attempt = now + START_RETRY_MS;
      return;
    }
    if (!_disc.begin(COMPANY_ID, _codec.groupMarker(), beacon_cb)) {
      _next_start_attempt = now + START_RETRY_MS;
      BRIDGE_DEBUG_PRINTLN("BLE: discovery failed to start, retrying later\n");
      return;
    }
    _link.begin(link_rx_cb, self, allow_cb);
    _transport_up = true;
    BRIDGE_DEBUG_PRINTLN("BLE: transport up, marker=0x%04X\n", (int)_codec.groupMarker());
  }

  _link.loop();

  /* A slot that went down loses its stamp. The next peer on that slot counts
     from its own sequence number, and a stamp left by the peer before it books
     either a false loss or a false restart. Metrics only. */
  for (uint8_t i = 0; i < NUM_STAMPS; i++) {
    uint8_t idx = (i == BleLink::MAX_LINKS) ? BleLink::INBOUND_LINK : i;
    if (_stamps[i].seq_valid && !_link.isUp(idx)) memset(&_stamps[i], 0, sizeof(_stamps[i]));
  }

  /* A link that never proved group membership. BleLink drops it and reports the
     address; the deny list is here, because the bridge owns the key. */
  ble_gap_addr_t failed;
  while (_link.takeAuthFailure(failed)) {
    _deny.add(failed.addr, millis());
    BRIDGE_DEBUG_PRINTLN("BLE: link never authenticated, address denied\n");
  }

  /* A connect attempt stopped the scanner. Arm it again whenever the link
     topology moved, or the node is deaf from here on. */
  if (_link.takeTopologyChanged()) _disc.requestRescan();

  /* The transport can take itself down when its receive path stops to answer.
     Rebuild it, and do not leave the bridge nominally up and permanently deaf.
     begin() is idempotent and arms the scanner from the start. */
  if (!_disc.isRunning()) {
    _transport_up = false;
    _next_start_attempt = millis() + START_RETRY_MS;
    BRIDGE_DEBUG_PRINTLN("BLE: transport went down, will restart\n");
    return;
  }

  unsigned long now = millis();

  /* Battery for the beacon, so a node can be triaged without a connection.
     Read here rather than pushed in from MyMesh: battery is a board property,
     and the repeater must not grow a bridge callback for it. */
  if ((long)(now - _next_batt_ms) >= 0) {
    _next_batt_ms = now + BATTERY_POLL_MS;
    _disc.setBattery(board.getBattMilliVolts());
  }

  if (_next_hb_ms == 0) {
    _next_hb_ms = now + HEARTBEAT_MS;
  } else if ((long)(now - _next_hb_ms) >= 0) {
    _next_hb_ms = now + HEARTBEAT_MS;
    sendHeartbeat();
  }

  _disc.loop();
}

void BLEBridge::beacon_cb(const ble_gap_addr_t& addr, int8_t rssi) {
  if (_instance) _instance->onBeacon(addr, rssi);
}

void BLEBridge::onBeacon(const ble_gap_addr_t& addr, int8_t rssi) {
  (void)rssi;
  /* The beacon carried our group marker, so this node speaks our protocol and
     probably holds our secret. It has not proved anything yet. Refuse an
     address that failed the group tag recently, and let BleLink decide whether
     we are the end that dials. */
  if (_deny.isDenied(addr.addr, millis())) {
    _num_dials_refused++;
    return;
  }
  _link.notePeer(addr);
}

bool BLEBridge::allow_cb(const ble_gap_addr_t& addr) {
  if (_instance == nullptr) return true;
  /* A peer that dials IN passes the same deny list as one that we dial. It is
     refused for the whole deny period, so a stranger cannot fail the group tag
     and come straight back to the single inbound slot. */
  if (_instance->_deny.isDenied(addr.addr, millis())) {
    _instance->_num_inbound_refused++;
    BRIDGE_DEBUG_PRINTLN("BLE: inbound peer is denied, disconnecting\n");
    return false;
  }
  return true;
}

void BLEBridge::link_rx_cb(const uint8_t* data, uint16_t len, uint8_t link_idx) {
  if (_instance) _instance->onLinkFrame(data, len, link_idx);
}

BLEBridge::LinkStamp* BLEBridge::stampFor(uint8_t link_idx) {
  if (link_idx == BleLink::INBOUND_LINK) return &_stamps[BleLink::MAX_LINKS];
  if (link_idx < BleLink::MAX_LINKS) return &_stamps[link_idx];
  return nullptr;
}

void BLEBridge::onLinkFrame(const uint8_t* data, uint16_t len, uint8_t link_idx) {
  const uint8_t* packet = nullptr;
  size_t packet_len = 0;
  uint16_t seq = 0;
  uint32_t timestamp = 0;

  BleBridgeFrame::ParseResult r =
      _codec.parse(data, len, &packet, &packet_len, &seq, &timestamp);

  if (r == BleBridgeFrame::PARSE_FOREIGN) {
    _num_foreign++;
    return;
  }

  if (r == BleBridgeFrame::PARSE_BAD_TAG) {
    /* BLE authenticates the LINK. The group tag is what says that this peer
       belongs to our bridge and not merely that it speaks the protocol.

       On a link that already authenticated, a bad tag means that bytes crossed
       an acknowledged connection and STILL failed. The controller does not
       deliver a corrupt payload, so that is our own framing -- the reassembly
       in BleLink -- and any sustained count is a fault.

       On a link that has NOT authenticated, this is the first frame, and it is
       a stranger. Drop the link and deny the address, so it cannot come
       straight back and take one of the three central slots again. */
    _num_bad_tag++;
    ble_gap_addr_t addr;
    bool have_addr = false;
    if (link_idx == BleLink::INBOUND_LINK) {
      have_addr = _link.getInboundAddr(addr);
    } else {
      bool up; int8_t rssi; uint32_t s, rcv, d;
      have_addr = _link.getLink(link_idx, addr, up, rssi, s, rcv, d);
    }
    if (have_addr) _deny.add(addr.addr, millis());
    _link.dropLink(link_idx);
    return;
  }

  /* The tag passed, so this peer is one of ours. That is the only evidence of
     group membership there is. Tell BleLink, so it stops the grace timer and
     keeps the slot. A heartbeat counts: it is tagged too, and it is what a
     quiet peer sends. */
  _link.markAuthed(link_idx);

  /* Sequence gaps, in uint16 arithmetic so the wrap at 65535 costs nothing. An
     advance of 1 means consecutive; anything larger is that many frames that we
     never saw. */
  LinkStamp* st = stampFor(link_idx);
  if (st != nullptr) {
    if (!st->seq_valid) {
      st->seq_valid = true;                    // first frame: nothing to compare
    } else {
      uint16_t advance = (uint16_t)(seq - st->last_seq);
      if (advance > SEQ_RESET_GAP) {
        // they restarted; do not blame the link
      } else if (advance > 1) {
        st->lost += (uint32_t)(advance - 1);
      }
    }
    st->last_seq = seq;
    st->frames++;
    st->skew_s = (int32_t)(timestamp - _rtc->getCurrentTime());
  }

  if (r == BleBridgeFrame::PARSE_HEARTBEAT) {
    _num_hb_rx++;
    return;
  }

  _num_rx_ok++;

#if WITH_STATUS_LED
  /* A bridged packet never reaches logRxRaw, because it goes straight inbound,
     so the LoRa hooks never see it. Blue is the bridge's own colour. */
  StatusLed::bleRx();
#endif
  BRIDGE_DEBUG_PRINTLN("BLE: RX, payload_len=%d\n", (int)packet_len);

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) return;
  if (pkt->readFrom(packet, (uint8_t)packet_len)) {
    onPacketReceived(pkt);
  } else {
    _mgr->free(pkt);
  }
}

void BLEBridge::sendHeartbeat() {
  size_t len = _codec.buildHeartbeat(_tx_frame, _tx_seq++, _rtc->getCurrentTime());
  /* Down every established link. sendKeepalive() excludes nothing, because a
     heartbeat is locally sourced. */
  _link.sendKeepalive(_tx_frame, (uint16_t)len);
}

void BLEBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !_transport_up) return;

  if (!packet) {
    BRIDGE_DEBUG_PRINTLN("BLE: TX invalid packet pointer\n");
    return;
  }

  if (_seen_packets.wasSeen(packet)) return;
  _seen_packets.markSeen(packet);

  /* writeTo() documents its destination as MAX_TRANS_UNIT and does not bound
     itself, so it gets a full-size buffer. To serialise straight into the frame
     would leave it MAX_PAYLOAD_SIZE bytes only, and a large packet would run
     off the end. Both buffers are members, because logTx calls this from deep
     in the dispatcher's call stack. */
  uint8_t packet_len = packet->writeTo(_staging);

  size_t frame_len = _codec.buildData(_tx_frame, _tx_seq, _rtc->getCurrentTime(),
                                      _staging, packet_len);
  if (frame_len == 0) {
    /* Too large. The codec counted it; see numTxOversize(). The sequence number
       is NOT consumed, because a gap in it means a frame that the peer never
       received, and this frame was never built. */
    BRIDGE_DEBUG_PRINTLN("BLE: TX packet too large (len=%d, max=%d)\n", (int)packet_len,
                         (int)BleBridgeFrame::MAX_PAYLOAD_SIZE);
    return;
  }
  _tx_seq++;

  if (_link.send(_tx_frame, (uint16_t)frame_len) > 0) {
    _num_sent++;
#if WITH_STATUS_LED
    StatusLed::bleTx();
#endif
    BRIDGE_DEBUG_PRINTLN("BLE: TX, len=%d\n", (int)packet_len);
  } else {
    /* No link is up, so the packet does not cross. Counted, because a bridge
       with no peer looks exactly like a bridge that works until somebody reads
       this number. */
    _num_tx_no_link++;
    BRIDGE_DEBUG_PRINTLN("BLE: TX with no link up\n");
  }
}

void BLEBridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}
