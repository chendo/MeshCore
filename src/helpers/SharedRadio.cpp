#include "SharedRadio.h"
#include <Arduino.h>

#if PKT_TRACE_ENTRIES
#include <SHA256.h>
#include <Packet.h>

// The packet hash of MeshCore, computed again from a raw over-the-air frame.
//
// Packet::calculatePacketHash() hashes the payload TYPE and then the payload
// bytes. For TRACE it also hashes path_len, because a TRACE packet may
// correctly visit a node more than one time. The function excludes the path, so
// that a packet keeps one identity while the mesh forwards it. This function
// copies that behaviour exactly. If the two functions ever become different,
// the trace would group the packets differently from the dedup tables. That
// result is worse than a trace with no hash at all.
//
// Returns false when the frame is too short to hold a payload.
static bool frameHash(const uint8_t* f, int len, uint8_t out[4]) {
  if (f == nullptr || len < 2) return false;
  uint8_t route = f[0] & 0x03;
  uint8_t type = (f[0] >> 2) & 0x0F;
  int o = 1;
  if (route == 0 || route == 3) o += 4;        // transport codes
  if (o >= len) return false;
  uint8_t path_byte = f[o++];
  uint8_t hops = path_byte & 63;
  uint8_t sz = (path_byte >> 6) + 1;
  o += hops * sz;
  if (o >= len) return false;                  // the frame has no payload bytes

  SHA256 sha;
  sha.update(&type, 1);
  if (type == PAYLOAD_TYPE_TRACE) sha.update(&path_byte, 1);
  sha.update(&f[o], len - o);
  uint8_t full[MAX_HASH_SIZE];
  sha.finalize(full, MAX_HASH_SIZE);
  memcpy(out, full, 4);
  return true;
}
#endif  // PKT_TRACE_ENTRIES

// ------------------------------------------------------------------ core

void SharedRadioCore::pump() {
  // While a port owns the transmitter, do not touch the real radio. recvRaw()
  // arms the RX again with startReceive() whenever state != STATE_RX. That
  // would stop a transmit that is in flight, and its TX-done interrupt would
  // never arrive.
  //
  // WATCHDOG. Because that rule gates pump(), a transmit that never finishes
  // takes the whole board off the air. Every identity goes deaf, not only the
  // sender. We saw this in the field. A repeater advert held the transmitter
  // for 3.4 hours. Its TX-done never arrived, AND its dispatcher never expired
  // it. Nothing received one packet in that time. A shared transmitter must
  // never be held for an unlimited time, whatever the reason. Therefore the
  // code takes it back.
  if (_tx_owner != nullptr) {
    if ((uint32_t)(millis() - _tx_started_ms) < TX_HOLD_LIMIT_MS) {
      return;                       // a normal transmit that is in flight
    }
    _tx_stuck = _tx_stuck + 1;
    int idx = portIndex(_tx_owner);
    pktLogAdd((int8_t)idx, nullptr, 0, 0, 0, PKT_FLAG_TX_FAIL,
              (int16_t)((millis() - _tx_started_ms) / 1000));   // aux = the seconds held
    _real->onSendFinished();        // release the radio. recvRaw() arms the RX again below.
    _tx_owner = nullptr;
  }

  _real->loop();   // the loop() of a port is a stub. This line services the real driver.

  retireConsumedFrames();

  // Deliver the locally transmitted frames to the sibling identities. See
  // setLoopback(). The code marks the sender first, so the sender never hears
  // itself.
  while (_lb_count > 0 && _rx_count < RX_SLOTS) {
    LbFrame& f = _lb[_lb_head];
    enqueueRx(f.buf, f.len, 12.0f, -20.0f, _cfg_cr, f.from >= 0 ? (1u << f.from) : 0);
    _lb_head = (_lb_head + 1) % LB_SLOTS;
    _lb_count--;
  }

  uint8_t tmp[MAX_TRANS_UNIT];
  int len = _real->recvRaw(tmp, sizeof(tmp));   // ALWAYS empty the radio

  // Report the RX decode and CRC failures as trace events. The driver only
  // counts them.
  if (_rx_err_fn != nullptr) {
    uint32_t errs = _rx_err_fn();
    int16_t code = _rx_err_code_fn ? _rx_err_code_fn() : 0;
    const uint8_t* bad = _rx_err_payload_fn ? _rx_err_payload_fn() : nullptr;
    int bad_len = _rx_err_len_fn ? (int)_rx_err_len_fn() : 0;
    while (_rx_err_seen < errs) {
      _rx_err_seen++;
      pktLogAdd(-1, bad, bad_len, (int8_t)(_real->getLastSNR() * 4), (int16_t)_real->getLastRSSI(),
                PKT_FLAG_RX_ERR, code);
    }
  }

  if (len > 0) {
    _last_rx_ms = millis();
    // Read the coding rate now. The register holds the coding rate of this
    // frame only until the modem decodes the next frame, and the identities
    // take the frames off the queue much later.
    enqueueRx(tmp, len, _real->getLastSNR(), _real->getLastRSSI(),
              _real ? _real->getLastRxCodingRate() : 0, 0);
    pktLogAdd(-1, tmp, len, (int8_t)(_real->getLastSNR() * 4), (int16_t)_real->getLastRSSI());
    _obs.observeRx(tmp, len, (int8_t)(_real->getLastSNR() * 4));    // the peers, hops and types
    if (_frame_hook) _frame_hook(tmp, len, false, _real->getLastSNR(), _real->getLastRSSI());
  }

  // RADIO HEALTH WATCHDOG.
  // The transceiver can wedge. Every RadioLib call then starts to fail. The
  // radio receives nothing and can transmit nothing. The driver samples the
  // noise floor only while it is truly in receive mode. Therefore the whole
  // radio goes quiet, and no part of the code reports an error. We saw this in
  // the field. The board was deaf for 3.4 hours after a transmit, and only a
  // reboot repaired it. Silence for this long is not a quiet band. It is a
  // broken radio.
  if (_reinit_fn != nullptr && _last_rx_ms != 0 &&
      (uint32_t)(millis() - _last_rx_ms) > RX_SILENCE_LIMIT_MS) {
    _radio_recoveries = _radio_recoveries + 1;
    _last_rx_ms = millis();          // do not retry in a tight loop
    _tx_owner = nullptr;
    _reinit_fn();
  }
}

// Put a frame in the queue for the identities to take. Returns false only when
// the code had to discard a frame to make space.
bool SharedRadioCore::enqueueRx(const uint8_t* bytes, int len, float snr, float rssi,
                                uint8_t cr, uint32_t consumed_init) {
  if (bytes == nullptr || len <= 0) return true;
  bool ok = true;
  if (_rx_count >= RX_SLOTS) {
    // Every identity is behind. To lose the OLDEST frame is the better choice,
    // because the newest frame is the frame that still propagates through the
    // mesh.
    _rx_head = (_rx_head + 1) % RX_SLOTS;
    _rx_count--;
    _rx_dropped = _rx_dropped + 1;
    ok = false;
  }
  RxFrame& f = _rx[(_rx_head + _rx_count) % RX_SLOTS];
  f.len = (uint8_t)(len > MAX_TRANS_UNIT ? MAX_TRANS_UNIT : len);
  memcpy(f.buf, bytes, f.len);
  f.snr = snr; f.rssi = rssi;
  f.cr = (cr >= 5 && cr <= 8) ? cr : 0;
  f.consumed = consumed_init | ~allPortsMask();   // an inactive port never takes a frame
  _rx_count++;
  return ok;
}

// Remove the frames at the front after every listening identity has taken
// them.
void SharedRadioCore::retireConsumedFrames() {
  while (_rx_count > 0) {
    RxFrame& f = _rx[_rx_head];
    if ((f.consumed & allPortsMask()) != allPortsMask()) break;
    _rx_head = (_rx_head + 1) % RX_SLOTS;
    _rx_count--;
  }
}

int SharedRadioCore::takeFrame(RadioPort* p, uint8_t* dst, int sz) {
  int idx = portIndex(p);
  if (idx < 0) return 0;
  uint32_t bit = (1u << idx);
  // A silenced identity must not receive. It must also not mark the frames as
  // taken, because the code could then never retire them.
  if ((_active_mask & bit) == 0) return 0;

  for (uint8_t k = 0; k < _rx_count; k++) {
    RxFrame& f = _rx[(_rx_head + k) % RX_SLOTS];
    if (f.consumed & bit) continue;              // this port has already taken it
    int len = f.len;
    if (len > sz) len = sz;
    memcpy(dst, f.buf, len);
    p->setLastMetadata(f.snr, f.rssi, f.cr);
    f.consumed |= bit;
    _port_rx[idx] = _port_rx[idx] + 1;
    _port_last_ms[idx] = millis();
    retireConsumedFrames();
    return len;
  }
  return 0;
}

bool SharedRadioCore::tryStartSend(RadioPort* p, const uint8_t* bytes, int len) {
  int p_idx = portIndex(p);
  if (_tx_owner != nullptr) {
    // A sibling identity holds the transmitter. The dispatcher backs off and
    // retries, so nothing showed this event before. The code now counts it,
    // both in total and for each identity, and writes a trace event.
    // Therefore you can see the contention between the roles on this board.
    _tx_contention = _tx_contention + 1;
    if (p_idx >= 0 && p_idx < MAX_PORTS) {
      _tx_contention_port[p_idx] = _tx_contention_port[p_idx] + 1;
      _claim_ms[p_idx] = millis();     // it wanted the air and did not get it
    }
    noteWait(TXWAIT_SIBLING);
    pktLogAdd((int8_t)p_idx, nullptr, 0, 0, 0, PKT_FLAG_TX_BUSY, (int16_t)portIndex(_tx_owner));
    return false;   // the transmitter is busy. The caller, a Dispatcher, discards the frame and retries.
  }

  // HARD DUTY-CYCLE LIMIT. A dispatcher that waits too long can override every
  // other rule in the send path. Its CAD-busy timeout force-sends after 4 s,
  // whatever isReceiving() reports. It cannot override this limit. The pool is
  // what keeps the node legal. Therefore the code refuses a frame that does not
  // fit, and the caller discards that frame. The node does not transmit above
  // the limit.
  refillBudget();
  uint32_t est = _real->getEstAirtimeFor(len);
  if (est > _budget_ms) {
    noteWait(TXWAIT_BUDGET);
    pktLogAdd((int8_t)p_idx, bytes, len, 0, 0, PKT_FLAG_TX_FAIL, -2);
    return false;
  }
  // A port that transmits over a live claim of higher priority reached this
  // point by a force-send through its own CAD-busy timeout. Count it, because
  // it is the one way to defeat the priority order below.
  if (higherPriorityClaimed(p_idx)) noteWait(TXWAIT_FORCED);

  // Apply the TX power of this persona before the radio keys up.
  if (_pwr_ctl != nullptr) {
    int8_t want = p->portTxPower();
    if (want != 0 && want != _applied_pwr) {
      _pwr_ctl->applyTxPower(want);
      _applied_pwr = want;
    }
  }
  bool ok = _real->startSendRaw(bytes, len);
  if (!ok) {
    // The radio REFUSED the send with a RadioLib error. Before this change the
    // code reported nothing. There was no trace entry and no counter.
    // Therefore hours of failed transmits left no evidence at all. This is a
    // transmit failure, so count it.
    _tx_refused = _tx_refused + 1;
    noteWait(TXWAIT_RADIO);
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0, PKT_FLAG_TX_FAIL, -1);
    return false;
  }
  {
    _budget_ms -= est;
    _tx_charged_ms += est;
    if (p_idx >= 0 && p_idx < MAX_PORTS) _claim_ms[p_idx] = 0;   // the port got the air
    _tx_owner = p;
    _tx_started_ms = millis();
    _tx_completed = false;
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0);
    // This is our own transmission. It has no SNR and no RSSI.
    if (_frame_hook) _frame_hook(bytes, len, true, 0, 0);
    int pidx = portIndex(p);
    if (pidx >= 0 && pidx < MAX_PORTS) {
      _port_tx[pidx] = _port_tx[pidx] + 1;    // the liveness of each identity
      _port_last_ms[pidx] = millis();
    }
    _obs.observeTx(bytes, len, pidx);   // the TX side of the relay confirmation

    if (_loopback && _num_ports > 1 && len > 0 && len <= MAX_TRANS_UNIT && _lb_count < LB_SLOTS) {
      LbFrame& f = _lb[(_lb_head + _lb_count) % LB_SLOTS];
      memcpy(f.buf, bytes, len);
      f.len = (uint8_t)len;
      f.from = (int8_t)portIndex(p);
      _lb_count++;
    }
  }
  return ok;
}

// ------------------------------------------------- pooled duty-cycle budget

void SharedRadioCore::setDutyCycle(float airtime_budget_factor, uint32_t window_ms) {
  if (airtime_budget_factor < 0) airtime_budget_factor = 0;
  if (window_ms == 0) window_ms = 3600000;
  _duty = 1.0f / (1.0f + airtime_budget_factor);
  _duty_window_ms = window_ms;
  _duty_max_ms = (uint32_t)(window_ms * _duty);
  _budget_ms = _duty_max_ms;               // the start of a window, as in Dispatcher::begin()
  _budget_last_ms = millis();
}

void SharedRadioCore::refillBudget() {
  uint32_t now = millis();
  uint32_t elapsed = now - _budget_last_ms;
  uint32_t refill = (uint32_t)(elapsed * _duty);
  if (refill == 0) return;                 // keep the remainder for the next call
  _budget_ms += refill;
  if (_budget_ms > _duty_max_ms) _budget_ms = _duty_max_ms;
  _budget_last_ms = now;
}

uint32_t SharedRadioCore::txBudgetMs() {
  refillBudget();
  return _budget_ms;
}

// --------------------------------------------------- cross-identity priority

void SharedRadioCore::setPortPriority(int idx, uint8_t pri) {
  if (idx >= 0 && idx < MAX_PORTS) _port_pri[idx] = pri;
}

uint8_t SharedRadioCore::portPriority(int idx) const {
  return (idx >= 0 && idx < MAX_PORTS) ? _port_pri[idx] : 1;
}

bool SharedRadioCore::higherPriorityClaimed(int idx) const {
  if (idx < 0 || idx >= MAX_PORTS) return false;
  uint32_t now = millis();
  for (int i = 0; i < _num_ports; i++) {
    if (i == idx) continue;
    if ((_active_mask & (1u << i)) == 0) continue;     // a silenced identity has no claim
    if (_port_pri[i] >= _port_pri[idx]) continue;      // ports of equal rank do not yield to each other
    if (_claim_ms[i] != 0 && (uint32_t)(now - _claim_ms[i]) < CLAIM_TTL_MS) return true;
  }
  return false;
}

// ------------------------------------------------------------- send gating

bool SharedRadioCore::portMustWait(RadioPort* p) {
  int idx = portIndex(p);

  // Control reaches this point only when the dispatcher of this port has a
  // packet in its queue and asks whether it may key up. Therefore the port
  // wanted the air, whatever reason below turns it away. Record the claim, so
  // that a port of higher rank that keeps losing the channel still goes in
  // front of the others when the channel clears.
  bool wait = false;
  int reason = -1;

  // The order below follows how far up the stack each obstacle sits. The code
  // tests the budget before the priority, and that order is intentional. When
  // the pool is empty, no port can transmit. A report that the lower-ranked
  // ports "yielded to the repeater" would then point at the wrong problem.
  refillBudget();
  if (txBusyForOthers(p)) {
    wait = true; reason = TXWAIT_SIBLING;
  } else if (_budget_ms < BUDGET_RESERVE_MS) {
    wait = true; reason = TXWAIT_BUDGET;
  } else if (higherPriorityClaimed(idx)) {
    wait = true; reason = TXWAIT_PRIORITY;
  } else if (_real->isReceiving()) {
    wait = true;
    // One bool holds three conditions. Ask the composition which one occurred.
    reason = _busy_probe ? _busy_probe() : TXWAIT_RX_PACKET;
    if (reason < TXWAIT_RX_PACKET || reason > TXWAIT_CAD) reason = TXWAIT_RX_PACKET;
  }

  if (wait) {
    noteWait(reason);
    if (idx >= 0 && idx < MAX_PORTS) _claim_ms[idx] = millis();
  }
  return wait;
}

void SharedRadioCore::pktLogAdd(int8_t dir, const uint8_t* bytes, int len, int8_t snr4, int16_t rssi, uint8_t flag, int16_t aux) {
  // The coding rate. For a receive, this is the value that the sender put in
  // the LoRa header of the frame that is still in the modem. The code reads it
  // now, before the next frame arrives. For a transmit, this is the value that
  // we are configured to send at. The synthetic entries carry no packet, so
  // they get 0. They also get 0 for the airtime.
  //
  // A receive that FAILED gets a coding rate only if its header survived. A
  // CRC mismatch is a damaged payload behind a good header. On a header error
  // the modem still holds the CR of the previous packet, and to report that
  // value would invent a fact about this frame.
  uint8_t cr = 0;
  // The airtime for this length. It is 0 for the synthetic entries that carry
  // no packet, for example TX-BUSY and TX-FAIL. The code prices a receive
  // whose CR it knows at THAT CR. For the same bytes, 4/8 spends 60% longer on
  // the channel than 4/5. Therefore, to cost the packet of a neighbour at our
  // own setting would be a true error in the one number that the
  // channel-occupancy view is built on.
  uint32_t air = 0;
  // When the build removes the trace, only the airtime sum below still needs
  // this value. The synthetic TX-FAIL and TX-BUSY rows then stop paying for a
  // read of the CR register.
#if PKT_TRACE_ENTRIES
  if (len > 0) {
#else
  if (len > 0 && flag == PKT_FLAG_OK) {
#endif
    bool hdr_trustworthy = (flag != PKT_FLAG_RX_ERR) || (aux == PKT_RX_ERR_CRC);
    if (dir < 0) {
      uint8_t rx_cr = (hdr_trustworthy && _real != nullptr) ? _real->getLastRxCodingRate() : 0;
      cr = (rx_cr >= 5 && rx_cr <= 8) ? rx_cr : 0;
    } else {
      cr = _cfg_cr;
    }
    if (dir < 0 && cr != 0 && _real != nullptr) air = _real->getEstAirtimeForCR(len, cr);
    else if (_real != nullptr) air = _real->getEstAirtimeFor(len);
  }

  if (flag == PKT_FLAG_OK) {
    // The node-wide time on air. The code keeps it here and not in a
    // Dispatcher, because one identity can account only for its own share of
    // the transmit. That fact is what makes a per-identity radio watchdog
    // wrong behind a shared radio.
    if (dir < 0) { _rx_total = _rx_total + 1; _rx_air_ms = _rx_air_ms + air; }
    else         { _tx_total = _tx_total + 1; _tx_air_ms = _tx_air_ms + air; }
  }

#if PKT_TRACE_ENTRIES
  PktLogEntry& e = _pkt_log[_pkt_seq % PKT_TRACE_ENTRIES];
  e.t_ms = millis();
  e.dir = dir;
  e.flag = flag;
  e.hdr = (len > 0 && bytes != nullptr) ? bytes[0] : 0;
  e.len = (uint8_t)(len > 255 ? 255 : len);
  e.snr4 = snr4; e.rssi = rssi; e.aux = aux;
  e.cr = cr;
  e.air_ms = (uint16_t)(air > 65535 ? 65535 : air);
  // Take the hash from the FULL frame, before the code truncates the raw
  // capture.
  e.hash_ok = frameHash(bytes, len, e.hash);
  if (!e.hash_ok) memset(e.hash, 0, sizeof(e.hash));
  e.raw_len = (uint8_t)(len > PKT_TRACE_RAW_CAP ? PKT_TRACE_RAW_CAP : len);
  if (e.raw_len > 0 && bytes != nullptr) memcpy(e.raw, bytes, e.raw_len); else e.raw_len = 0;
  e.seq = _pkt_seq + 1;   // the code writes this last. A reader treats seq==0 or an old seq as invalid.
  _pkt_seq = _pkt_seq + 1;
#else
  (void)bytes; (void)snr4; (void)rssi; (void)cr;
#endif
}

int SharedRadioCore::pktLogCopy(PktLogEntry* out, int max_entries, uint32_t after_seq) {
#if PKT_TRACE_ENTRIES
  // The ring has one writer. A torn read can affect only the entry that the
  // writer overwrites, and the seq check removes that entry.
  uint32_t newest = _pkt_seq;
  uint32_t oldest = newest > PKT_TRACE_ENTRIES ? newest - PKT_TRACE_ENTRIES : 0;
  if (after_seq < oldest) after_seq = oldest;
  int n = 0;
  for (uint32_t s = after_seq + 1; s <= newest && n < max_entries; s++) {
    PktLogEntry e = _pkt_log[(s - 1) % PKT_TRACE_ENTRIES];
    if (e.seq != s) continue;   // the writer overwrote it during the copy
    out[n++] = e;
  }
  return n;
#else
  (void)out; (void)max_entries; (void)after_seq;
  return 0;   // the build removed the trace. Report no history, not old or invented rows.
#endif
}

bool SharedRadioCore::isSendCompleteFor(RadioPort* p) {
  if (_tx_owner != p) return true;   // this is not our send. Report done, so that no port hangs.
  bool done = _real->isSendComplete();
  if (done) _tx_completed = true;
  return done;
}

void SharedRadioCore::onSendFinishedFor(RadioPort* p) {
  if (_tx_owner == p) {
    if (!_tx_completed) {
      // The dispatcher stopped at its send expiry, before TX-done arrived.
      pktLogAdd((int8_t)portIndex(p), nullptr, 0, 0, 0, PKT_FLAG_TX_FAIL);
    }
    _real->onSendFinished();
    _tx_owner = nullptr;
  }
}

void SharedRadioCore::applyRadioPolicy(bool recalibrate) {
  bool cad = false;
  int thresh = 0;          // 0 means that the RSSI check is off
  for (int i = 0; i < _num_ports; i++) {
    if ((_active_mask & (1u << i)) == 0) continue;   // a silenced identity has no vote
    if (_ports[i]->wantsCAD()) cad = true;
    int t = _ports[i]->wantedThreshold();
    // A lower threshold that is not zero defers to weaker signals. Therefore
    // it is the more careful of the two. Any identity that asks for the check
    // turns the check on.
    if (t != 0 && (thresh == 0 || t < thresh)) thresh = t;
  }

  if (_cad_applied != (int8_t)cad) {
    _cad_applied = (int8_t)cad;
    _real->setCADEnabled(cad);
  }

  // A recalibration resets the sampling window of the noise floor. Three
  // dispatchers that ask every 2s would restart that window three times as
  // frequently as the stock design intends. Therefore the code limits the rate
  // back to the original one. There is one exception. When the threshold
  // itself changed, the new value must take effect immediately.
  bool thresh_changed = (thresh != _thresh_applied);
  if (recalibrate &&
      (thresh_changed || _last_calib_ms == 0 ||
       (uint32_t)(millis() - _last_calib_ms) >= CALIB_MIN_INTERVAL_MS)) {
    _thresh_applied = thresh;
    _last_calib_ms = millis();
    _real->triggerNoiseFloorCalibrate(thresh);
  }
}

void SharedRadioCore::setPortActive(int idx, bool active) {
  if (idx < 0 || idx >= MAX_PORTS) return;
  uint32_t bit = (1u << idx);
  if (active) {
    // The frames that are already in the queue arrived while this port was
    // silent. Mark them as taken, so that the port does not start with a
    // backlog of packets that it never heard.
    for (uint8_t k = 0; k < _rx_count; k++) _rx[(_rx_head + k) % RX_SLOTS].consumed |= bit;
    _active_mask |= bit;
  } else {
    _active_mask &= ~bit;
    // Never wait on a silenced port. Its frames could then never retire.
    for (uint8_t k = 0; k < _rx_count; k++) _rx[(_rx_head + k) % RX_SLOTS].consumed |= bit;
    retireConsumedFrames();
    if (_tx_owner != nullptr && portIndex(_tx_owner) == idx) {
      _real->onSendFinished();    // do not leave the transmitter with no owner
      _tx_owner = nullptr;
    }
  }
  applyRadioPolicy(false);        // a silenced identity no longer votes
}

bool SharedRadioCore::portActive(int idx) const {
  return idx >= 0 && idx < MAX_PORTS && (_active_mask & (1u << idx)) != 0;
}

void SharedRadioCore::beginRealOnce() {
  if (_real_begun) return;
  _real_begun = true;
  _real->begin();   // this attaches the radio ISR for RX-done and TX-done, and does other init work
}

// ------------------------------------------------------------------ port

void RadioPort::begin() {
  if (_core) _core->beginRealOnce();
}

int RadioPort::recvRaw(uint8_t* bytes, int sz) {
  return _core ? _core->takeFrame(this, bytes, sz) : 0;
}

bool RadioPort::startSendRaw(const uint8_t* bytes, int len) {
  return _core ? _core->tryStartSend(this, bytes, len) : false;
}

bool RadioPort::isSendComplete() {
  return _core ? _core->isSendCompleteFor(this) : true;
}

void RadioPort::onSendFinished() {
  if (_core) _core->onSendFinishedFor(this);
}

bool RadioPort::isReceiving() {
  // This answers more than the question "is the channel busy". It reports
  // every reason why this identity may not key up now. The back-off of the
  // dispatcher is the only control that we have from below mesh::Radio. See
  // SharedRadioCore::portMustWait().
  return _core ? _core->portMustWait(this) : false;
}

uint32_t RadioPort::getEstAirtimeFor(int len_bytes) {
  return _core ? _core->real()->getEstAirtimeFor(len_bytes) : 0;
}

uint32_t RadioPort::getEstAirtimeForCR(int len_bytes, uint8_t cr) {
  return _core ? _core->real()->getEstAirtimeForCR(len_bytes, cr) : 0;
}

float RadioPort::packetScore(float snr, int packet_len) {
  return _core ? _core->real()->packetScore(snr, packet_len) : 0;
}

int RadioPort::getNoiseFloor() const {
  return _core ? _core->real()->getNoiseFloor() : 0;
}

// These two functions record what the prefs of this identity want. The core
// then combines the three requests into one setting for the shared radio.
void RadioPort::triggerNoiseFloorCalibrate(int threshold) {
  _thresh_want = threshold;
  if (_core) _core->applyRadioPolicy(true);
}

void RadioPort::setCADEnabled(bool enable) {
  _cad_want = enable;
  if (_core) _core->applyRadioPolicy(false);
}

void RadioPort::resetAGC() {
  // resetAGC() puts the radio into a warm sleep. The periodic AGC timer of
  // another port must not do that while a transmit is in flight.
  if (_core && !_core->txInFlight()) _core->real()->resetAGC();
}
