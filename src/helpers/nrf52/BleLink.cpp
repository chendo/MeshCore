#include "BleLink.h"

#include <string.h>

/* Vendor UUIDs for the bridge service. Random 128-bit, so nothing else claims
   them; the base is shared and only the 16-bit slot differs. */
static const uint8_t BRIDGE_SVC_UUID[16] = {
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
  0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x7A, 0x40, 0x6E
};
static const uint8_t BRIDGE_CHR_UUID[16] = {
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
  0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x7A, 0x40, 0x6E
};

/* Named rather than temporaries: BLEService s_svc(BLEUuid(x)) parses as a
   function declaration, not an object. */
static BLEUuid s_svc_uuid(BRIDGE_SVC_UUID);
static BLEUuid s_chr_uuid(BRIDGE_CHR_UUID);

/* Peripheral side: what a peer dialling US talks to. */
static BLEService        s_svc(s_svc_uuid);
static BLECharacteristic s_chr(s_chr_uuid);

/* Central side: Bluefruit binds a client service to one connection at a time,
   so simultaneous outward links need their own instances rather than a shared
   pair re-pointed per connection. */
static BLEClientService        s_clt[BleLink::MAX_LINKS] = {
  BLEClientService(s_svc_uuid), BLEClientService(s_svc_uuid), BLEClientService(s_svc_uuid)
};
static BLEClientCharacteristic s_cchr[BleLink::MAX_LINKS] = {
  BLEClientCharacteristic(s_chr_uuid), BLEClientCharacteristic(s_chr_uuid),
  BLEClientCharacteristic(s_chr_uuid)
};

static BleLink* s_instance = nullptr;

/* ATT payload on the default 23-byte MTU. Raising the MTU would buy fewer
   fragments at the cost of SoftDevice RAM the role counts already exhausted. */
static const uint16_t CHUNK = 20;

bool BleLink::begin(rx_handler_t handler, const ble_gap_addr_t& self_addr) {
  if (_running) return true;
  s_instance = this;
  _handler = handler;
  memcpy(&_self, &self_addr, sizeof(_self));
  memset(_links, 0, sizeof(_links));

  s_svc.begin();                       // must precede its characteristics
  s_chr.setProperties(CHR_PROPS_WRITE_WO_RESP | CHR_PROPS_NOTIFY);
  s_chr.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  s_chr.setMaxLen(CHUNK);
  s_chr.setWriteCallback(written_cb);
  s_chr.begin();

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    s_clt[i].begin();
    s_cchr[i].setNotifyCallback(notify_cb);
    s_cchr[i].begin();
  }

  Bluefruit.Central.setConnectCallback(connect_cb);
  Bluefruit.Central.setDisconnectCallback(disconnect_cb);

  _running = true;
  return true;
}

bool BleLink::weInitiateTo(const ble_gap_addr_t& peer) const {
  /* Addresses are little-endian on the wire; compare from the most significant
     byte so the ordering matches how a human reads them. Equality cannot happen
     between two distinct devices. */
  for (int i = 5; i >= 0; i--) {
    if (_self.addr[i] != peer.addr[i]) return _self.addr[i] < peer.addr[i];
  }
  return false;
}

void BleLink::notePeer(const ble_gap_addr_t& addr) {
  if (!_running) return;
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state != EMPTY && memcmp(_links[i].addr.addr, addr.addr, 6) == 0) return;
  }
  /* Only take a slot for peers we are responsible for dialling. One that
     outranks us will dial in, and holding a slot open for it would waste one of
     the three we have. */
  if (!weInitiateTo(addr)) return;

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state == EMPTY) {
      memset(&_links[i], 0, sizeof(_links[i]));
      memcpy(&_links[i].addr, &addr, sizeof(addr));
      _links[i].state = IDLE;
      _links[i].conn = BLE_CONN_HANDLE_INVALID;
      _links[i].backoff_ms = BACKOFF_MIN_MS;
      return;
    }
  }
}

void BleLink::loop() {
  if (!_running) return;
  unsigned long now = millis();

  /* Drop a link that has gone quiet. Heartbeats put a frame on every link each
     interval, so a minute of silence is not a lull -- it is a connection the
     link layer still believes in and the peer has stopped using. Disconnecting
     returns it to IDLE, where the normal backoff redials it. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != UP || l.last_rx_ms == 0) continue;
    /* Signed, for the same reason as the scanner's silence check: last_rx_ms is
       written from BLE event context and can land just ahead of the millis()
       this loop captured, which unsigned would read as a very old link and
       disconnect a perfectly healthy one. */
    if ((long)(now - l.last_rx_ms) < (long)LINK_IDLE_LIMIT_MS) continue;
    BLEConnection* c = Bluefruit.Connection(l.conn);
    if (c != nullptr) c->disconnect();
    l.drops++;
    l.state = IDLE;
    l.conn = BLE_CONN_HANDLE_INVALID;
    l.last_rx_ms = 0;
  }

  /* Prove group membership, or lose the slot.
     Connecting says nothing about belonging to our bridge: BleLink dials on
     discovery and address tie-break, and the group tag is only checked per
     frame afterwards. So a node with a DIFFERENT secret connects perfectly
     happily, occupies a central slot, and has every frame discarded -- an
     expensive way to achieve nothing, and invisible without the split bad-tag
     counter. A correctly-keyed peer authenticates within one heartbeat
     interval; anything that has not by now is not ours. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != UP || l.authed || l.up_ms == 0) continue;
    if ((long)(now - l.up_ms) < (long)AUTH_GRACE_MS) continue;
    BLEConnection* c = Bluefruit.Connection(l.conn);
    if (c != nullptr) c->disconnect();
    l.unauthed_drops++;
    l.state = IDLE;
    l.conn = BLE_CONN_HANDLE_INVALID;
    l.last_rx_ms = 0;
    /* Back off hard rather than redialling immediately: a peer that cannot
       authenticate now will not authenticate in two seconds either. */
    l.backoff_ms = BACKOFF_MAX_MS;
    l.next_try_ms = now + l.backoff_ms;
  }

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != IDLE) continue;
    if (l.next_try_ms != 0 && (long)(now - l.next_try_ms) < 0) continue;

    /* Back off before marking the attempt, so a refused connect does not spin.
       Bluefruit reports the outcome through the connect callback, which cannot
       tell us WHICH link it was for until the handle arrives -- so the state
       machine has to tolerate an attempt that simply never completes. */
    l.state = CONNECTING;
    l.next_try_ms = now + l.backoff_ms;
    if (l.backoff_ms < BACKOFF_MAX_MS) l.backoff_ms = (uint16_t)(l.backoff_ms * 2 > BACKOFF_MAX_MS
                                                                ? BACKOFF_MAX_MS : l.backoff_ms * 2);
    /* Whether or not this succeeds, the SoftDevice has now stopped scanning --
       sd_ble_gap_connect() does that unconditionally. Flag it so the caller
       re-arms, or the node is deaf from here on. */
    _topology_changed = true;
    if (!Bluefruit.Central.connect(&l.addr)) l.state = IDLE;
  }
}

int BleLink::findByConn(uint16_t conn) const {
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state != EMPTY && _links[i].conn == conn) return i;
  }
  return -1;
}

void BleLink::onConnected(uint16_t conn) {
  /* Bluefruit's central callback does not say which peer it dialled, so match
     the connection back to a link by address. */
  BLEConnection* c = Bluefruit.Connection(conn);
  if (c == nullptr) return;
  ble_gap_addr_t peer = c->getPeerAddr();

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != CONNECTING || memcmp(l.addr.addr, peer.addr, 6) != 0) continue;

    l.conn = conn;
    l.state = DISCOVERING;
    l.rx_expect = l.rx_have = 0; l.rx_hdr_have = 0;

    if (s_clt[i].discover(conn) && s_cchr[i].discover() && s_cchr[i].enableNotify()) {
      l.state = UP;
      l.last_rx_ms = millis();               // grace period before the idle check
      l.up_ms = millis();                    // start of the authentication window
      l.authed = false;                      // GATT discovery proves protocol, not group
      l.backoff_ms = BACKOFF_MIN_MS;         // a link that worked starts fresh
    } else {
      /* Connected to something that is not a bridge peer, or discovery failed.
         Drop it rather than hold a slot for a link that can never carry. */
      c->disconnect();
      l.state = IDLE;
      l.conn = BLE_CONN_HANDLE_INVALID;
    }
    return;
  }
}

void BleLink::onDisconnected(uint16_t conn, uint8_t reason) {
  int idx = findByConn(conn);
  if (idx < 0) return;
  Link& l = _links[idx];
  if (l.state == UP) l.drops++;
  _topology_changed = true;
  l.state = IDLE;
  l.conn = BLE_CONN_HANDLE_INVALID;
  l.rx_expect = l.rx_have = 0; l.rx_hdr_have = 0;
  (void)reason;                              // surfaced through getLink's counters
}

void BleLink::markAuthed(uint8_t idx) {
  if (idx == NO_LINK) { _in_authed = true; return; }
  if (idx < MAX_LINKS) _links[idx].authed = true;
}

void BleLink::feed(Link& l, uint8_t idx, const uint8_t* data, uint16_t len) {
  reassemble(l.rx_hdr, l.rx_hdr_have, l.rx_expect, l.rx_have, l.rx_buf,
             l.rx_started_ms, l.rx_resyncs, l.recv, l.last_rx_ms, idx, data, len);
}

/* SYNC-marked, length-prefixed reassembly, resynchronising and self-limiting.
   One implementation for both directions: the outward links and the inbound
   peer previously had separate copies of this loop, and therefore separate
   copies of the same bug. */
void BleLink::reassemble(uint8_t* hdr, uint8_t& hdr_have,
                         uint16_t& expect, uint16_t& have, uint8_t* buf,
                         unsigned long& started_ms, uint32_t& resyncs,
                         uint32_t& recv, unsigned long& last_rx_ms,
                         uint8_t idx, const uint8_t* data, uint16_t len) {
  /* Abandon a frame that stopped arriving. writeFragmented() gives up when a
     chunk write fails, having already sent the header, so without this we wait
     forever for a tail that is never coming -- and consume the NEXT frame as
     that tail. Signed comparison, as everywhere else in this file. */
  if ((hdr_have != 0 || expect != 0)
      && (long)(millis() - started_ms) > (long)RX_STALE_MS) {
    hdr_have = 0; expect = 0; have = 0; resyncs++;
  }

  while (len > 0) {
    if (expect == 0) {
      /* Collect the header a byte at a time, hunting for SYNC. Byte-wise
         because a header can straddle two writes -- the old code returned with
         fewer than two bytes left and silently dropped them. Hunting because
         after a truncated frame the stream is misaligned, and the marker is the
         only way back to a frame boundary. */
      while (len > 0 && hdr_have < HDR_SIZE) {
        uint8_t b = *data++; len--;
        if (hdr_have == 0 && b != FRAME_SYNC) { resyncs++; continue; }
        hdr[hdr_have++] = b;
      }
      if (hdr_have < HDR_SIZE) { started_ms = millis(); return; }   // resume on the next write

      expect = (uint16_t)hdr[1] | ((uint16_t)hdr[2] << 8);
      hdr_have = 0;
      have = 0;
      started_ms = millis();
      if (expect == 0 || expect > MAX_FRAME) {   // a SYNC that was really payload
        expect = 0; resyncs++;
        continue;
      }
    }

    uint16_t want = expect - have;
    uint16_t take = len < want ? len : want;
    memcpy(&buf[have], data, take);
    have += take; data += take; len -= take;

    if (have == expect) {
      recv++;
      last_rx_ms = millis();
      if (_handler) _handler(buf, expect, idx);
      expect = 0; have = 0;
    }
  }
}

void BleLink::onNotify(uint8_t idx, const uint8_t* data, uint16_t len) {
  if (idx < MAX_LINKS) feed(_links[idx], idx, data, len);
}

void BleLink::onWritten(uint16_t conn, const uint8_t* data, uint16_t len) {
  /* A peer dialled US. Reassembled separately from the outward links: it has no
     Link slot, because we did not choose it and cannot dial it back. */
  if (_in_conn != conn) {
    _in_conn = conn;
    _in_expect = _in_have = 0; _in_hdr_have = 0;
  }
  static unsigned long s_in_last_rx_ms = 0;    // unused here; the peer link has no idle timer
  reassemble(_in_hdr, _in_hdr_have, _in_expect, _in_have, _in_buf,
             _in_started_ms, _in_resyncs, _in_recv, s_in_last_rx_ms, NO_LINK, data, len);
}

/* One chunk, waiting for TX credit rather than giving up on it.

   Write-without-response consumes a SoftDevice TX buffer per packet, and
   BLEClientCharacteristic::write() simply breaks out and returns 0 when none is
   free ("if (!conn->getWriteCmdPacket()) break;"). writeFragmented pushes
   ceil(len/20) packets back to back with no flow control, so anything bigger
   than a few credits ran the pool dry mid-frame -- and the old code treated
   that as fatal and abandoned the frame with its header already on the wire.
   The receiver was then left holding a partial frame, consumed the next frame
   as its missing tail, and handed the bridge a spliced packet that failed its
   HMAC. That is the entire 34% link loss: heartbeats fit in one credit and
   always arrived, multi-packet mesh frames did not.

   Credits are returned as connection events complete, so the wait is bounded by
   the connection interval (20-30ms here), not by anything unbounded. */
bool BleLink::writeChunk(uint8_t idx, const uint8_t* p, uint16_t n) {
  for (uint8_t attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
    if (s_cchr[idx].write(p, n) == n) return true;
    delay(2);                      // let a connection event hand credits back
  }
  return false;
}

bool BleLink::notifyChunk(const uint8_t* p, uint16_t n) {
  for (uint8_t attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
    if (s_chr.notify(p, n)) return true;
    delay(2);
  }
  return false;
}

bool BleLink::writeFragmented(uint8_t idx, const uint8_t* data, uint16_t len) {
  uint8_t first[CHUNK];
  first[0] = FRAME_SYNC;
  first[1] = (uint8_t)(len & 0xFF);
  first[2] = (uint8_t)(len >> 8);
  uint16_t n = (uint16_t)(len < CHUNK - HDR_SIZE ? len : CHUNK - HDR_SIZE);
  memcpy(&first[HDR_SIZE], data, n);
  if (!writeChunk(idx, first, (uint16_t)(n + HDR_SIZE))) { _tx_abandoned++; return false; }

  uint16_t off = n;
  while (off < len) {
    uint16_t take = (uint16_t)((len - off) < CHUNK ? (len - off) : CHUNK);
    if (!writeChunk(idx, &data[off], take)) {
      /* Still possible after all the retries. Counted, because a frame
         abandoned here is the one thing that can desynchronise the peer -- and
         the SYNC marker means it now costs one frame instead of the stream. */
      _tx_abandoned++;
      return false;
    }
    off += take;
  }
  return true;
}

uint8_t BleLink::sendKeepalive(const uint8_t* data, uint16_t len) {
  return send(data, len, NO_LINK);
}

uint8_t BleLink::send(const uint8_t* data, uint16_t len, uint8_t except) {
  if (!_running || len == 0 || len > MAX_FRAME) return 0;
  uint8_t n = 0;

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state != UP || i == except) continue;
    if (writeFragmented(i, data, len)) { _links[i].sent++; n++; }
  }

  /* The inbound peer, if one is attached. Notified rather than written to,
     since on that link the roles are the other way round. */
  if (_in_conn != BLE_CONN_HANDLE_INVALID && except != NO_LINK - 1) {
    uint8_t hdr[CHUNK];
    hdr[0] = FRAME_SYNC;
    hdr[1] = (uint8_t)(len & 0xFF); hdr[2] = (uint8_t)(len >> 8);
    uint16_t f = (uint16_t)(len < CHUNK - HDR_SIZE ? len : CHUNK - HDR_SIZE);
    memcpy(&hdr[HDR_SIZE], data, f);
    /* Same credit exhaustion as the client path -- notifications draw on the
       same TX pool, and the old code simply broke out of the loop mid-frame. */
    if (notifyChunk(hdr, (uint16_t)(f + HDR_SIZE))) {
      uint16_t off = f;
      bool ok = true;
      while (off < len) {
        uint16_t take = (uint16_t)((len - off) < CHUNK ? (len - off) : CHUNK);
        if (!notifyChunk(&data[off], take)) { ok = false; break; }
        off += take;
      }
      if (ok) n++; else _tx_abandoned++;
    } else {
      _tx_abandoned++;
    }
  }
  return n;
}

uint8_t BleLink::numUp() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_LINKS; i++) if (_links[i].state == UP) n++;
  if (_in_conn != BLE_CONN_HANDLE_INVALID) n++;
  return n;
}

bool BleLink::getLink(uint8_t idx, ble_gap_addr_t& addr, bool& up, int8_t& rssi,
                      uint32_t& sent, uint32_t& recv, uint32_t& drops) const {
  if (idx >= MAX_LINKS || _links[idx].state == EMPTY) return false;
  const Link& l = _links[idx];
  memcpy(&addr, &l.addr, sizeof(addr));
  up = (l.state == UP);
  rssi = 0;                                  // filled by the RSSI report, if started
  sent = l.sent; recv = l.recv; drops = l.drops;
  return true;
}

void BleLink::connect_cb(uint16_t conn) {
  if (s_instance) s_instance->onConnected(conn);
}
void BleLink::disconnect_cb(uint16_t conn, uint8_t reason) {
  if (s_instance) s_instance->onDisconnected(conn, reason);
}
void BleLink::notify_cb(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
  if (!s_instance) return;
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (chr == &s_cchr[i]) { s_instance->onNotify(i, data, len); return; }
  }
}
void BleLink::written_cb(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)chr;
  if (s_instance) s_instance->onWritten(conn, data, len);
}
