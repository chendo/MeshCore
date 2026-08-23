#include "BleLink.h"

#include <string.h>

static BleLink* s_instance = nullptr;

/* The largest single write. This follows the MTU that the stack negotiated, and
   not the 23-byte minimum: at MTU 247 a whole bridge frame is one write and
   never becomes fragments, which removes the entire class of desynchronisation
   faults that this transport had. The backend gives up MTU before it gives up
   connection slots, so this can still come back as 20 on a node that is short
   of RAM. Fragmentation still works there, with more packets. */
static const uint16_t MAX_CHUNK = 244;          // MTU 247 - 3 bytes of ATT overhead
static uint16_t s_chunk = 20;                   // resolved in begin(), after the stack is up
#define CHUNK s_chunk

bool BleLink::begin(rx_handler_t handler, const BleAddr& self_addr,
                    allow_handler_t allow) {
  if (_running) return true;
  s_instance = this;
  _handler = handler;
  _allow = allow;
  memcpy(&_self, &self_addr, sizeof(_self));
  memset(_links, 0, sizeof(_links));

  /* Resolve the write size from what the stack negotiated. The backend has
     already run its ladder, so this is the real MTU and not a hope. */
  {
    uint16_t m = Backend::stackMtu();
    if (m < 23) m = 23;
    uint16_t c = (uint16_t)(m - 3);             // ATT opcode + handle
    s_chunk = c > MAX_CHUNK ? MAX_CHUNK : c;
  }

  /* The backend registers the GATT service once for the life of the process,
     because `set bridge.secret` restarts the bridge and thus runs this again. */
  Backend::begin(CHUNK, connect_cb, disconnect_cb, notify_cb, written_cb);

  _running = true;
  return true;
}

void BleLink::resetLink(Link& l) {
  l.state = IDLE;
  l.conn = Backend::NO_CONN;
  l.last_rx_ms = 0;
  l.txq_count = 0; l.txq_head = 0; l.tx_off = 0;
  l.rx_expect = l.rx_have = 0; l.rx_hdr_have = 0;
  l.authed = false; l.up_ms = 0;
}

void BleLink::resetInbound() {
  /* Hold the handle for a moment. A disconnect is asynchronous, so a write
     from the peer we just dropped still reaches onWritten(), and without this
     record it is adopted again with a fresh authentication window. That is
     exactly the squat that the drop was for. */
  if (_in_conn != Backend::NO_CONN) {
    _in_dropped_conn = _in_conn;
    _in_dropped_ms = millis();
  }
  _in_conn = Backend::NO_CONN;
  _in_txq_count = 0; _in_txq_head = 0; _in_tx_off = 0;
  _in_expect = _in_have = 0; _in_hdr_have = 0;
  _in_authed = false; _in_up_ms = 0;
  /* The next peer must start its own silence timer, not inherit this one. */
  _in_last_rx_ms = 0;
}

void BleLink::end() {
  if (!_running) return;
  _running = false;

  /* Disconnect for real. To stop the bridge and leave its GATT links up is
     worse than useless: the peer holds a connection to a node that never
     answers, and a node advertises only while a peripheral slot is FREE (see
     BleDiscovery), so a node with a single peripheral slot then stays INVISIBLE
     with its bridge switched off. It has no CLI and no DFU. That is exactly how
     the production node became impossible to flash. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state == UP || l.state == DISCOVERING || l.state == CONNECTING) {
      Backend::disconnect(l.conn);
    }
    resetLink(l);
    l.state = EMPTY;                         // the slot is free for another peer
  }

  if (_in_conn != Backend::NO_CONN) Backend::disconnect(_in_conn);
  resetInbound();
  _auth_fail_count = 0; _auth_fail_head = 0;
  _topology_changed = true;                  // the caller must arm the scanner again
}

bool BleLink::weInitiateTo(const BleAddr& peer) const {
  /* Addresses are little-endian on the air. Compare from the most significant
     byte, so the order matches how a person reads an address. Two distinct
     devices cannot be equal. */
  for (int i = 5; i >= 0; i--) {
    if (_self.addr[i] != peer.addr[i]) return _self.addr[i] < peer.addr[i];
  }
  return false;
}

void BleLink::notePeer(const BleAddr& addr) {
  if (!_running) return;
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state != EMPTY && memcmp(_links[i].addr.addr, addr.addr, 6) == 0) return;
  }
  /* Take a slot only for a peer that we are responsible to dial. A peer that
     outranks us dials in, and a slot held open for it wastes one of the three
     that we have. */
  if (!weInitiateTo(addr)) return;

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state == EMPTY) {
      memset(&_links[i], 0, sizeof(_links[i]));
      memcpy(&_links[i].addr, &addr, sizeof(addr));
      _links[i].state = IDLE;
      _links[i].conn = Backend::NO_CONN;
      _links[i].backoff_ms = BACKOFF_MIN_MS;
      return;
    }
  }
}

void BleLink::noteAuthFailure(const BleAddr& addr) {
  if (_auth_fail_count >= AUTH_FAIL_DEPTH) return;    // the bridge is not reading
  uint8_t slot = (uint8_t)((_auth_fail_head + _auth_fail_count) % AUTH_FAIL_DEPTH);
  memcpy(&_auth_fail[slot], &addr, sizeof(addr));
  _auth_fail_count++;
}

bool BleLink::takeAuthFailure(BleAddr& addr) {
  if (_auth_fail_count == 0) return false;
  memcpy(&addr, &_auth_fail[_auth_fail_head], sizeof(addr));
  _auth_fail_head = (uint8_t)((_auth_fail_head + 1) % AUTH_FAIL_DEPTH);
  _auth_fail_count--;
  return true;
}

void BleLink::dropLink(uint8_t idx) {
  if (idx == INBOUND_LINK) {
    if (_in_conn == Backend::NO_CONN) return;
    Backend::disconnect(_in_conn);
    resetInbound();
    _topology_changed = true;
    return;
  }
  if (idx >= MAX_LINKS) return;
  Link& l = _links[idx];
  if (l.state == EMPTY) return;
  Backend::disconnect(l.conn);
  l.unauthed_drops++;
  resetLink(l);
  /* Back off hard, and do not dial again at once. A peer that cannot
     authenticate now will not authenticate in two seconds either. */
  l.backoff_ms = BACKOFF_MAX_MS;
  l.next_try_ms = millis() + l.backoff_ms;
  _topology_changed = true;
}

void BleLink::checkInbound() {
  if (_in_conn == Backend::NO_CONN) return;
  if (Backend::connected(_in_conn)) return;
  /* The peer went away. Forget it, or numUp() over-counts for ever and every
     notify() goes to a handle that no longer exists. The source of this port
     never cleared the handle, because nothing else looked at it. */
  resetInbound();
  _topology_changed = true;
}

void BleLink::loop() {
  if (!_running) return;

  /* Let the backend deliver anything it had to hold. A stack whose events
     arrive on a task that must not block cannot run GATT discovery from its own
     connect callback, so it queues the event and hands it over here instead,
     where a blocking call costs only a slow loop pass. The nRF52 backend has
     nothing to hold and this compiles away. */
  Backend::poll();

  unsigned long now = millis();

  checkInbound();
  sweepInbound();

  /* Drop a link that went quiet. Heartbeats put a frame on every link in each
     interval, so a minute of silence is not a lull. It is a connection that the
     link layer still believes in and the peer stopped to use. A disconnect
     returns the link to IDLE, where the normal backoff dials it again. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != UP || l.last_rx_ms == 0) continue;
    /* Signed, for the same reason as the scanner's silence check: the BLE event
       context writes last_rx_ms and can land it just ahead of the millis() that
       this loop captured. Unsigned, that reads as a very old link and
       disconnects a perfectly healthy one. */
    if ((long)(now - l.last_rx_ms) < (long)LINK_IDLE_LIMIT_MS) continue;
    Backend::disconnect(l.conn);
    l.drops++;
    resetLink(l);
  }

  /* Prove group membership, or lose the slot.
     A connection says nothing about membership of our bridge. BleLink dials on
     a beacon match and an address tie-break, and the group tag is checked for
     each frame afterwards. A node with a DIFFERENT secret thus connects
     happily, occupies a central slot, and has every frame discarded. That is an
     expensive way to achieve nothing, and it is invisible without the split
     bad-tag counter. A peer with the correct key authenticates within one
     heartbeat interval; anything that has not by now is not ours. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != UP || l.authed || l.up_ms == 0) continue;
    if ((long)(now - l.up_ms) < (long)AUTH_GRACE_MS) continue;
    noteAuthFailure(l.addr);
    dropLink(i);
  }
  if (_in_conn != Backend::NO_CONN && !_in_authed && _in_up_ms != 0
      && (long)(now - _in_up_ms) >= (long)AUTH_GRACE_MS) {
    BleAddr a;
    if (getInboundAddr(a)) noteAuthFailure(a);
    dropLink(INBOUND_LINK);
  }

  /* The inbound peer gets the same silence limit as an outward link, and for
     the same reason. There is exactly ONE inbound slot, so a peer that
     authenticates and then goes quiet holds all of it, and no other peer can
     dial us at all. */
  if (_in_conn != Backend::NO_CONN && _in_last_rx_ms != 0
      && (long)(now - _in_last_rx_ms) >= (long)LINK_IDLE_LIMIT_MS) {
    dropLink(INBOUND_LINK);
  }

  /* Push queued frames out as credits allow. Nothing blocks: whatever the
     SoftDevice does not take now resumes on the next pass. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state == UP) drain(_links[i], i);
  }
  drainInbound();

  /* A dial that never completed. NimBLE can leave an async connect outstanding
     for ever, and while it is outstanding ble_gap_disc() returns BLE_HS_EBUSY:
     one unanswered dial makes the node permanently deaf, because it can no
     longer scan and so never sees a beacon again. Bounding it here, rather
     than in one backend, keeps the recovery in the state machine that owns the
     slot. cancelDial() frees the radio; the backend that does not need it says
     so and returns false. */
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != CONNECTING) continue;
    if ((long)(now - l.connect_deadline_ms) < 0) continue;
    Backend::cancelDial();
    l.state = IDLE;
    _topology_changed = true;      // the caller must arm the scanner again
  }

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != IDLE) continue;
    if (l.next_try_ms != 0 && (long)(now - l.next_try_ms) < 0) continue;

    /* Back off before we mark the attempt, so a refused connect does not spin.
       Bluefruit reports the result through the connect callback, which cannot
       say WHICH link it was for until the handle arrives. The state machine
       must thus tolerate an attempt that simply never completes. */
    l.state = CONNECTING;
    l.next_try_ms = now + l.backoff_ms;
    l.connect_deadline_ms = now + CONNECT_LIMIT_MS;
    if (l.backoff_ms < BACKOFF_MAX_MS) l.backoff_ms = (uint16_t)(l.backoff_ms * 2 > BACKOFF_MAX_MS
                                                                ? BACKOFF_MAX_MS : l.backoff_ms * 2);
    /* Whether or not this succeeds, the SoftDevice has now stopped the scan.
       sd_ble_gap_connect() does that unconditionally. Flag it, so the caller
       arms the scanner again; otherwise the node is deaf from here on. */
    _topology_changed = true;
    if (!Backend::dial(l.addr)) l.state = IDLE;
  }
}

int BleLink::findByConn(uint16_t conn) const {
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state != EMPTY && _links[i].conn == conn) return i;
  }
  return -1;
}

void BleLink::onConnected(uint16_t conn) {
  /* The central callback does not say which peer it dialled, so match the
     connection back to a link by address. */
  BleAddr peer;
  if (!Backend::peerAddr(conn, peer)) return;

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    Link& l = _links[i];
    if (l.state != CONNECTING || memcmp(l.addr.addr, peer.addr, 6) != 0) continue;

    l.conn = conn;
    l.state = DISCOVERING;
    l.rx_expect = l.rx_have = 0; l.rx_hdr_have = 0;

    if (Backend::discoverLink(i, conn)) {
      l.state = UP;
      l.last_rx_ms = millis();               // a grace period before the idle check
      l.up_ms = millis();                    // start of the authentication window
      l.authed = false;                      // GATT discovery proves protocol, not group
      l.backoff_ms = BACKOFF_MIN_MS;         // a link that worked starts fresh
    } else {
      /* We connected to something that is not a bridge peer, or discovery
         failed. Drop it, and do not hold a slot for a link that can carry
         nothing. */
      Backend::disconnect(conn);
      l.state = IDLE;
      l.conn = Backend::NO_CONN;
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
  resetLink(l);
  (void)reason;                              // reported through getLink's counters
}

void BleLink::markAuthed(uint8_t idx) {
  if (idx == INBOUND_LINK) { _in_authed = true; return; }
  if (idx < MAX_LINKS) _links[idx].authed = true;
}

void BleLink::feed(Link& l, uint8_t idx, const uint8_t* data, uint16_t len) {
  reassemble(l.rx_hdr, l.rx_hdr_have, l.rx_expect, l.rx_have, l.rx_buf,
             l.rx_started_ms, l.rx_resyncs, l.recv, l.last_rx_ms, idx, data, len);
}

/* Reassembly with a SYNC marker and a length prefix, which resynchronises and
   limits itself. One implementation for both directions: the outward links and
   the inbound peer had separate copies of this loop, and thus separate copies
   of the same fault. */
void BleLink::reassemble(uint8_t* hdr, uint8_t& hdr_have,
                         uint16_t& expect, uint16_t& have, uint8_t* buf,
                         unsigned long& started_ms, uint32_t& resyncs,
                         uint32_t& recv, unsigned long& last_rx_ms,
                         uint8_t idx, const uint8_t* data, uint16_t len) {
  /* Abandon a frame that stopped to arrive. A sender can still stop in the
     middle of a frame -- a link that drops between chunks, or a peer with older
     firmware -- so without this we wait for ever for a tail that never comes,
     and we consume the NEXT frame as that tail. Signed comparison, as
     everywhere else in this file. */
  if ((hdr_have != 0 || expect != 0)
      && (long)(millis() - started_ms) > (long)RX_STALE_MS) {
    hdr_have = 0; expect = 0; have = 0; resyncs++;
  }

  while (len > 0) {
    if (expect == 0) {
      /* Collect the header one byte at a time and hunt for SYNC. Byte by byte
         because a header can straddle two writes: the old code returned with
         fewer than two bytes left and dropped them in silence. Hunt, because
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

bool BleLink::adoptInbound(uint16_t conn) {
  /* Refuse the handle that we dropped a moment ago. Its disconnect is still
     in flight, and to adopt it again would grant another grace window. */
  if (conn == _in_dropped_conn
      && (long)(millis() - _in_dropped_ms) < (long)IN_DROP_HOLD_MS) return false;

  /* The deny list gates a peer that dials IN as well as one that we dial.
     Without this the deny list stops nothing on this side: a peer whose
     first frame failed the group tag simply connects again and takes the
     single inbound slot back. */
  BleAddr peer;
  if (_allow != nullptr && Backend::peerAddr(conn, peer) && !_allow(peer)) {
    Backend::disconnect(conn);
    return false;
  }
  _in_conn = conn;
  _in_expect = _in_have = 0; _in_hdr_have = 0;
  _in_authed = false;
  _in_last_rx_ms = 0;                        // start of this peer's silence timer
  _in_up_ms = millis();                      // start of the authentication window
  _topology_changed = true;
  return true;
}

/* Find a peer that dialled in and has written nothing.
   A write used to be the ONLY event that made an inbound peer known, so a peer
   that connected and stayed silent was never adopted, never timed out and
   never denied. It held the one inbound slot for as long as its radio stayed
   in range. This sweep closes that, and the peer then meets the authentication
   grace and the deny list that every other peer meets.

   A poll, and not a callback. On nRF52 the peripheral connect callback is a
   SINGLE slot, and SerialBLEInterface already holds it. Two owners of one slot
   means whichever begin() runs last takes the callback from the other in
   silence, and that fault appears only in a build combination that nobody has
   tried. loop() already runs on every main pass, so a poll costs one short
   walk over the connection handles and owes nothing to anybody.

   peripheralConnAt() reports only a connection on which we hold the PERIPHERAL
   role and which is live, so an outward link of our own can never match here.

   A secured or bonded connection belongs to the CLI or to DFU. Both need MITM
   encryption before they carry a byte, and a bridge peer never pairs, so this
   is what keeps the sweep away from the CLI. */
void BleLink::sweepInbound() {
#if BLE_LINK_SILENT_SWEEP
  if (_in_conn != Backend::NO_CONN) return;    // the slot is already ours

  for (uint8_t i = 0; i < Backend::SWEEP_SLOTS; i++) {
    uint16_t h = Backend::peripheralConnAt(i);
    if (h == Backend::NO_CONN) continue;
    if (!Backend::exists(h)) continue;
    if (Backend::paired(h)) continue;                 // the CLI, or DFU
    if (adoptInbound(h)) return;
  }
#endif
}

void BleLink::onWritten(uint16_t conn, const uint8_t* data, uint16_t len) {
  /* A peer dialled US. Reassembled apart from the outward links: it has no Link
     slot, because we did not choose it and cannot dial it back. */
  if (_in_conn != conn && !adoptInbound(conn)) return;
  reassemble(_in_hdr, _in_hdr_have, _in_expect, _in_have, _in_buf,
             _in_started_ms, _in_resyncs, _in_recv, _in_last_rx_ms,
             INBOUND_LINK, data, len);
}

uint8_t BleLink::sendKeepalive(const uint8_t* data, uint16_t len) {
  return send(data, len, NO_LINK);
}

/* Copy a framed datagram into one destination's queue. This never blocks. A
   full queue is a drop, and it is counted, which is honest backpressure and not
   an unbounded wait. */
bool BleLink::enqueue(Link& l, const uint8_t* data, uint16_t len) {
  if (l.txq_count >= TXQ_DEPTH) { l.tx_dropped++; return false; }
  uint8_t slot = (uint8_t)((l.txq_head + l.txq_count) % TXQ_DEPTH);
  uint8_t* b = l.txq[slot];
  b[0] = FRAME_SYNC;
  b[1] = (uint8_t)(len & 0xFF);
  b[2] = (uint8_t)(len >> 8);
  memcpy(&b[HDR_SIZE], data, len);
  l.txq_len[slot] = (uint16_t)(len + HDR_SIZE);
  l.txq_count++;
  return true;
}

/* Push as much of the queue as the SoftDevice takes, then stop. loop() calls
   this on every pass, and whatever is left resumes next time from tx_off. */
void BleLink::drain(Link& l, uint8_t idx) {
  while (l.txq_count > 0) {
    const uint8_t* b = l.txq[l.txq_head];
    uint16_t total = l.txq_len[l.txq_head];

    /* Clamp to what THIS connection negotiated, and not to what we asked the
       stack for at boot. Give write() more than the peer's ATT payload and it
       splits the buffer internally, then returns `len - remaining` -- a PARTIAL
       count -- if that split runs out of TX credits part way. To treat a
       partial as "nothing sent" and retry from the same offset puts the
       transmitted bytes on the air a SECOND time, in the middle of a frame, and
       the peer's reassembled frame then fails its HMAC.

       That was the residual link loss. The link layer acknowledges and
       retransmits until delivery, exactly as this class relies on. It delivered
       everything faithfully, and we handed it duplicated bytes. One packet for
       each write restores the all-or-nothing behaviour that the loop below
       assumes. */
    uint16_t cap = CHUNK;
    uint16_t conn_mtu = Backend::connMtu(l.conn);
    if (conn_mtu != 0) {
      uint16_t mp = (uint16_t)(conn_mtu - 3);
      if (mp < cap) cap = mp;
    }

    while (l.tx_off < total) {
      uint16_t rem = (uint16_t)(total - l.tx_off);
      uint16_t take = rem < cap ? rem : cap;
      /* One attempt. No credit means no credit: return and resume next pass.
         Advance by what the stack ACTUALLY accepted, so a partial can never go
         out twice even if the stack splits despite the clamp. */
      uint16_t wrote = Backend::writeLink(idx, &b[l.tx_off], take);
      l.tx_off = (uint16_t)(l.tx_off + wrote);
      if (wrote != take) return;
    }

    l.txq_head = (uint8_t)((l.txq_head + 1) % TXQ_DEPTH);
    l.txq_count--;
    l.tx_off = 0;
    l.sent++;
  }
}

uint8_t BleLink::send(const uint8_t* data, uint16_t len, uint8_t except) {
  if (!_running || len == 0 || len > MAX_FRAME) return 0;
  uint8_t n = 0;

  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (_links[i].state != UP || i == except) continue;
    if (enqueue(_links[i], data, len)) n++;
  }

  /* The inbound peer, if one is attached. We notify it rather than write to it,
     because on that link the roles are the other way round. */
  if (_in_conn != Backend::NO_CONN && except != INBOUND_LINK) {
    if (enqueueInbound(data, len)) n++;
  }
  return n;
}

bool BleLink::enqueueInbound(const uint8_t* data, uint16_t len) {
  if (_in_txq_count >= TXQ_DEPTH) { _tx_dropped_in++; return false; }
  uint8_t slot = (uint8_t)((_in_txq_head + _in_txq_count) % TXQ_DEPTH);
  uint8_t* b = _in_txq[slot];
  b[0] = FRAME_SYNC;
  b[1] = (uint8_t)(len & 0xFF);
  b[2] = (uint8_t)(len >> 8);
  memcpy(&b[HDR_SIZE], data, len);
  _in_txq_len[slot] = (uint16_t)(len + HDR_SIZE);
  _in_txq_count++;
  return true;
}

void BleLink::drainInbound() {
  if (_in_conn == Backend::NO_CONN) {
    _in_txq_count = 0; _in_txq_head = 0; _in_tx_off = 0;   // peer gone; nothing to send
    return;
  }
  while (_in_txq_count > 0) {
    const uint8_t* b = _in_txq[_in_txq_head];
    uint16_t total = _in_txq_len[_in_txq_head];
    /* The same clamp, and notify() needs it MORE than write() does. It splits
       internally too, but it returns only a bool: "if (!conn->getHvnPacket())
       return false;" fires AFTER it may already have queued packets, so a
       partial send looks the same as no send and the bytes cannot be accounted
       for. To keep every notify to a single packet is the only way to make
       false reliably mean that nothing went out. */
    uint16_t cap = CHUNK;
    uint16_t conn_mtu = Backend::connMtu(_in_conn);
    if (conn_mtu != 0) {
      uint16_t mp = (uint16_t)(conn_mtu - 3);
      if (mp < cap) cap = mp;
    }

    while (_in_tx_off < total) {
      uint16_t rem = (uint16_t)(total - _in_tx_off);
      uint16_t take = rem < cap ? rem : cap;
      if (!Backend::notifyInbound(_in_conn, &b[_in_tx_off], take)) return;  // no credit
      _in_tx_off = (uint16_t)(_in_tx_off + take);
    }
    _in_txq_head = (uint8_t)((_in_txq_head + 1) % TXQ_DEPTH);
    _in_txq_count--;
    _in_tx_off = 0;
    _in_sent++;
  }
}

uint8_t BleLink::numUp() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_LINKS; i++) if (_links[i].state == UP) n++;
  if (_in_conn != Backend::NO_CONN) n++;
  return n;
}

bool BleLink::isUp(uint8_t idx) const {
  if (idx == INBOUND_LINK) return _in_conn != Backend::NO_CONN;
  return idx < MAX_LINKS && _links[idx].state == UP;
}

bool BleLink::getInboundAddr(BleAddr& addr, uint32_t* rx_age_s,
                             uint32_t* queued) const {
  if (_in_conn == Backend::NO_CONN) return false;
  if (!Backend::peerAddr(_in_conn, addr)) return false;
  /* The same figure as getLink() reports, and for the same reason: counters
     alone cannot tell a peer that carries traffic from one that went quiet an
     hour ago. Signed, because the BLE event context writes _in_last_rx_ms. */
  if (rx_age_s) {
    *rx_age_s = (_in_last_rx_ms == 0) ? 0xFFFFFFFF
              : (uint32_t)(((long)(millis() - _in_last_rx_ms)) / 1000);
  }
  if (queued) *queued = _in_txq_count;
  return true;
}

bool BleLink::getLink(uint8_t idx, BleAddr& addr, bool& up, int8_t& rssi,
                      uint32_t& sent, uint32_t& recv, uint32_t& drops,
                      uint32_t* rx_age_s, uint32_t* queued) const {
  if (idx >= MAX_LINKS || _links[idx].state == EMPTY) return false;
  const Link& l = _links[idx];
  memcpy(&addr, &l.addr, sizeof(addr));
  up = (l.state == UP);
  rssi = 0;                                  // filled by the RSSI report, if started
  sent = l.sent; recv = l.recv; drops = l.drops;
  /* Seconds since anything last arrived on this link. Counters alone cannot
     tell a link that carries traffic from a link that is nominally up and has
     been silent for an hour: the supervision timeout notices a radio that
     vanished, not a peer that went quiet. Signed, because the BLE event context
     writes last_rx_ms and can land it just ahead of this millis(). */
  if (rx_age_s) {
    *rx_age_s = (l.last_rx_ms == 0) ? 0xFFFFFFFF
              : (uint32_t)(((long)(millis() - l.last_rx_ms)) / 1000);
  }
  if (queued) *queued = l.txq_count;
  return true;
}

void BleLink::connect_cb(uint16_t conn) {
  if (s_instance) s_instance->onConnected(conn);
}
void BleLink::disconnect_cb(uint16_t conn, uint8_t reason) {
  if (s_instance) s_instance->onDisconnected(conn, reason);
}
void BleLink::notify_cb(uint8_t link_idx, const uint8_t* data, uint16_t len) {
  if (s_instance) s_instance->onNotify(link_idx, data, len);
}
void BleLink::written_cb(uint16_t conn, const uint8_t* data, uint16_t len) {
  if (s_instance) s_instance->onWritten(conn, data, len);
}
