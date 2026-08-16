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
    l.rx_expect = l.rx_have = 0;

    if (s_clt[i].discover(conn) && s_cchr[i].discover() && s_cchr[i].enableNotify()) {
      l.state = UP;
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
  l.state = IDLE;
  l.conn = BLE_CONN_HANDLE_INVALID;
  l.rx_expect = l.rx_have = 0;
  (void)reason;                              // surfaced through getLink's counters
}

/* Length-prefixed reassembly. The link is reliable and ordered, so a two-byte
   header and a running count is all the framing needed -- no sequence numbers,
   no gap handling, no timeouts. */
void BleLink::feed(Link& l, uint8_t idx, const uint8_t* data, uint16_t len) {
  while (len > 0) {
    if (l.rx_expect == 0) {
      if (len < 2) return;                   // a header never splits in practice
      l.rx_expect = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
      data += 2; len -= 2;
      l.rx_have = 0;
      if (l.rx_expect == 0 || l.rx_expect > MAX_FRAME) { l.rx_expect = 0; return; }
    }
    uint16_t want = l.rx_expect - l.rx_have;
    uint16_t take = len < want ? len : want;
    memcpy(&l.rx_buf[l.rx_have], data, take);
    l.rx_have += take; data += take; len -= take;

    if (l.rx_have == l.rx_expect) {
      l.recv++;
      if (_handler) _handler(l.rx_buf, l.rx_expect, idx);
      l.rx_expect = l.rx_have = 0;
    }
  }
}

void BleLink::onNotify(uint8_t idx, const uint8_t* data, uint16_t len) {
  if (idx < MAX_LINKS) feed(_links[idx], idx, data, len);
}

void BleLink::onWritten(uint16_t conn, const uint8_t* data, uint16_t len) {
  /* A peer dialled US. Reassembled separately from the outward links: it has no
     Link slot, because we did not choose it and cannot dial it back. */
  if (_in_conn != conn) { _in_conn = conn; _in_expect = _in_have = 0; }
  while (len > 0) {
    if (_in_expect == 0) {
      if (len < 2) return;
      _in_expect = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
      data += 2; len -= 2; _in_have = 0;
      if (_in_expect == 0 || _in_expect > MAX_FRAME) { _in_expect = 0; return; }
    }
    uint16_t want = _in_expect - _in_have;
    uint16_t take = len < want ? len : want;
    memcpy(&_in_buf[_in_have], data, take);
    _in_have += take; data += take; len -= take;
    if (_in_have == _in_expect) {
      _in_recv++;
      if (_handler) _handler(_in_buf, _in_expect, NO_LINK);
      _in_expect = _in_have = 0;
    }
  }
}

bool BleLink::writeFragmented(uint8_t idx, const uint8_t* data, uint16_t len) {
  uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint8_t first[CHUNK];
  memcpy(first, hdr, 2);
  uint16_t n = (uint16_t)(len < CHUNK - 2 ? len : CHUNK - 2);
  memcpy(&first[2], data, n);
  if (s_cchr[idx].write(first, (uint16_t)(n + 2)) == 0) return false;

  uint16_t off = n;
  while (off < len) {
    uint16_t take = (uint16_t)((len - off) < CHUNK ? (len - off) : CHUNK);
    if (s_cchr[idx].write(&data[off], take) == 0) return false;
    off += take;
  }
  return true;
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
    hdr[0] = (uint8_t)(len & 0xFF); hdr[1] = (uint8_t)(len >> 8);
    uint16_t f = (uint16_t)(len < CHUNK - 2 ? len : CHUNK - 2);
    memcpy(&hdr[2], data, f);
    if (s_chr.notify(hdr, (uint16_t)(f + 2))) {
      uint16_t off = f;
      while (off < len) {
        uint16_t take = (uint16_t)((len - off) < CHUNK ? (len - off) : CHUNK);
        if (!s_chr.notify(&data[off], take)) break;
        off += take;
      }
      n++;
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
