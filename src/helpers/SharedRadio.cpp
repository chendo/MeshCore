#include "SharedRadio.h"
#include <Arduino.h>

#if PKT_TRACE_ENTRIES
#include <SHA256.h>
#include <Packet.h>

// MeshCore's packet hash, recomputed from a raw over-the-air frame.
//
// Packet::calculatePacketHash() hashes the payload TYPE followed by the payload
// bytes (plus path_len for TRACE, which legitimately revisits nodes). It
// deliberately excludes the path, so the same packet keeps one identity as it is
// forwarded. This mirrors that exactly — if the two ever diverge, the trace
// would group packets differently from the dedup tables, which is worse than
// showing no hash at all.
//
// Returns false when the frame is too short to locate a payload.
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
  if (o >= len) return false;                  // no payload bytes present

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
  // While a port owns the transmitter, leave the real radio alone: recvRaw()
  // re-arms RX (startReceive()) whenever state != STATE_RX, which would abort
  // an in-flight transmit and its TX-done interrupt would never arrive.
  //
  // WATCHDOG: because pump() is gated on that, a transmit that never finishes
  // takes the whole board off air — every identity goes deaf, not just the
  // sender. Observed in the field: a repeater advert held the transmitter for
  // 3.4 hours (its TX-done never arrived AND its dispatcher never expired it),
  // and nothing received a single packet in that time. A shared transmitter
  // must never be held indefinitely, whatever the reason, so take it back.
  if (_tx_owner != nullptr) {
    if ((uint32_t)(millis() - _tx_started_ms) < TX_HOLD_LIMIT_MS) {
      return;                       // normal in-flight transmit
    }
    _tx_stuck = _tx_stuck + 1;
    int idx = portIndex(_tx_owner);
    pktLogAdd((int8_t)idx, nullptr, 0, 0, 0, PKT_FLAG_TX_FAIL,
              (int16_t)((millis() - _tx_started_ms) / 1000));   // aux = seconds held
    _real->onSendFinished();        // release the radio; recvRaw() re-arms RX below
    _tx_owner = nullptr;
  }

  _real->loop();   // ports' loop() is a stub; the real driver is serviced here

  retireConsumedFrames();

  // Deliver locally-transmitted frames to the sibling identities (see
  // setLoopback()); the sender is pre-marked so it never hears itself.
  while (_lb_count > 0 && _rx_count < RX_SLOTS) {
    LbFrame& f = _lb[_lb_head];
    enqueueRx(f.buf, f.len, 12.0f, -20.0f, _cfg_cr, f.from >= 0 ? (1u << f.from) : 0);
    _lb_head = (_lb_head + 1) % LB_SLOTS;
    _lb_count--;
  }

  uint8_t tmp[MAX_TRANS_UNIT];
  int len = _real->recvRaw(tmp, sizeof(tmp));   // ALWAYS drain the radio

  // surface RX decode/CRC failures (driver only counts them) as trace events
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
    // Read now: the register holds this frame's coding rate only until the next
    // one is decoded, and the identities take frames off the queue much later.
    enqueueRx(tmp, len, _real->getLastSNR(), _real->getLastRSSI(),
              _real ? _real->getLastRxCodingRate() : 0, 0);
    pktLogAdd(-1, tmp, len, (int8_t)(_real->getLastSNR() * 4), (int16_t)_real->getLastRSSI());
    _obs.observeRx(tmp, len, (int8_t)(_real->getLastSNR() * 4));    // peers, hops, types
    if (_frame_hook) _frame_hook(tmp, len, false, _real->getLastSNR(), _real->getLastRSSI());
  }

  // RADIO HEALTH WATCHDOG.
  // The transceiver can wedge: every RadioLib call starts failing, so nothing
  // is received, nothing can be transmitted, and — because the driver only
  // samples the noise floor while it is actually in receive mode — the whole
  // radio simply goes quiet with no error anywhere. Observed in the field: the
  // board was deaf for 3.4 hours after a transmit, and recovered only on
  // reboot. Silence for this long is not a quiet band, it is a broken radio.
  if (_reinit_fn != nullptr && _last_rx_ms != 0 &&
      (uint32_t)(millis() - _last_rx_ms) > RX_SILENCE_LIMIT_MS) {
    _radio_recoveries = _radio_recoveries + 1;
    _last_rx_ms = millis();          // don't retry in a tight loop
    _tx_owner = nullptr;
    _reinit_fn();
  }
}

// Queue a frame for the identities to consume. Returns false only if it had to
// discard something to make room.
bool SharedRadioCore::enqueueRx(const uint8_t* bytes, int len, float snr, float rssi,
                                uint8_t cr, uint32_t consumed_init) {
  if (bytes == nullptr || len <= 0) return true;
  bool ok = true;
  if (_rx_count >= RX_SLOTS) {
    // Every identity has fallen behind. Losing the OLDEST frame is the better
    // trade: the newest is the one still propagating through the mesh.
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
  f.consumed = consumed_init | ~allPortsMask();   // inactive ports never consume
  _rx_count++;
  return ok;
}

// Drop frames from the front once every listening identity has taken them.
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
  // A silenced identity must not receive (and must not mark frames consumed,
  // or they could never retire).
  if ((_active_mask & bit) == 0) return 0;

  for (uint8_t k = 0; k < _rx_count; k++) {
    RxFrame& f = _rx[(_rx_head + k) % RX_SLOTS];
    if (f.consumed & bit) continue;              // this port already had it
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
    // A sibling identity holds the transmitter. The dispatcher just backs off
    // and retries, so this used to be completely invisible — count it (overall
    // and per identity) and log a trace event so contention between the roles
    // on this board can actually be seen.
    _tx_contention = _tx_contention + 1;
    if (p_idx >= 0 && p_idx < MAX_PORTS) {
      _tx_contention_port[p_idx] = _tx_contention_port[p_idx] + 1;
      _claim_ms[p_idx] = millis();     // it wanted the air and did not get it
    }
    noteWait(TXWAIT_SIBLING);
    pktLogAdd((int8_t)p_idx, nullptr, 0, 0, 0, PKT_FLAG_TX_BUSY, (int16_t)portIndex(_tx_owner));
    return false;   // transmitter busy -> caller (Dispatcher) will drop & retry
  }

  // HARD DUTY-CYCLE CAP. Everything else in the send path can be overridden by
  // a dispatcher that has run out of patience — its CAD-busy timeout force-sends
  // after 4 s regardless of what isReceiving() says. This cannot be: the pool is
  // what keeps the node legal, so a frame that does not fit is refused outright,
  // and the caller drops it rather than transmitting over the limit.
  refillBudget();
  uint32_t est = _real->getEstAirtimeFor(len);
  if (est > _budget_ms) {
    noteWait(TXWAIT_BUDGET);
    pktLogAdd((int8_t)p_idx, bytes, len, 0, 0, PKT_FLAG_TX_FAIL, -2);
    return false;
  }
  // A port that transmits over a live higher-priority claim got there by
  // force-sending through its own CAD-busy timeout; count it, since it is the
  // one way the priority ordering below can be defeated.
  if (higherPriorityClaimed(p_idx)) noteWait(TXWAIT_FORCED);

  // apply this persona's TX power before keying up
  if (_pwr_ctl != nullptr) {
    int8_t want = p->portTxPower();
    if (want != 0 && want != _applied_pwr) {
      _pwr_ctl->applyTxPower(want);
      _applied_pwr = want;
    }
  }
  bool ok = _real->startSendRaw(bytes, len);
  if (!ok) {
    // The radio REFUSED the send (RadioLib error). This used to be completely
    // silent — no trace entry, no counter — which is why hours of failed
    // transmits left no evidence at all. It is a transmit failure: count it.
    _tx_refused = _tx_refused + 1;
    noteWait(TXWAIT_RADIO);
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0, PKT_FLAG_TX_FAIL, -1);
    return false;
  }
  {
    _budget_ms -= est;
    _tx_charged_ms += est;
    if (p_idx >= 0 && p_idx < MAX_PORTS) _claim_ms[p_idx] = 0;   // it got the air
    _tx_owner = p;
    _tx_started_ms = millis();
    _tx_completed = false;
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0);
    // our own transmission — no SNR/RSSI exist for it
    if (_frame_hook) _frame_hook(bytes, len, true, 0, 0);
    int pidx = portIndex(p);
    if (pidx >= 0 && pidx < MAX_PORTS) {
      _port_tx[pidx] = _port_tx[pidx] + 1;    // per-identity liveness
      _port_last_ms[pidx] = millis();
    }
    _obs.observeTx(bytes, len, pidx);   // relay confirmation, TX side

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
  _budget_ms = _duty_max_ms;               // start of a window, same as Dispatcher::begin()
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
    if (_port_pri[i] >= _port_pri[idx]) continue;      // equal ranks do not yield to each other
    if (_claim_ms[i] != 0 && (uint32_t)(now - _claim_ms[i]) < CLAIM_TTL_MS) return true;
  }
  return false;
}

// ------------------------------------------------------------- send gating

bool SharedRadioCore::portMustWait(RadioPort* p) {
  int idx = portIndex(p);

  // Reaching here means this port's dispatcher has a packet queued and is
  // asking whether it may key up — so whatever turns it away below, it wanted
  // the air. Record the claim so a higher-ranked port that keeps losing the
  // channel still gets in front of the others when it clears.
  bool wait = false;
  int reason = -1;

  // Ordered by how far up the stack the obstacle sits. The budget is tested
  // before priority deliberately: when the pool is dry nobody can transmit, and
  // reporting the lower-ranked ports as "yielded to the repeater" would point
  // at the wrong problem entirely.
  refillBudget();
  if (txBusyForOthers(p)) {
    wait = true; reason = TXWAIT_SIBLING;
  } else if (_budget_ms < BUDGET_RESERVE_MS) {
    wait = true; reason = TXWAIT_BUDGET;
  } else if (higherPriorityClaimed(idx)) {
    wait = true; reason = TXWAIT_PRIORITY;
  } else if (_real->isReceiving()) {
    wait = true;
    // one bool, three conditions — ask the composition which one fired
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
  // Coding rate: for a receive, whatever the sender put in the LoRa header of
  // the frame still sitting in the modem (read now, before the next one lands);
  // for a transmit, what we are configured to send at. The synthetic entries
  // that carry no packet get 0, as they do for airtime.
  //
  // A receive that FAILED gets one only if its header survived — a CRC mismatch
  // is a mangled payload behind a good header, but on a header error the modem
  // still holds the previous packet's CR, and reporting that would be inventing
  // a fact about this one.
  uint8_t cr = 0;
  // Airtime for this length (0 for the synthetic entries that carry no packet,
  // e.g. TX-BUSY / TX-FAIL). A receive whose CR we know is priced at THAT CR —
  // 4/8 spends 60% longer on the channel than 4/5 for the same bytes, so
  // costing a neighbour's packet at our own setting would be a real error in
  // the one number the channel-occupancy view is built on.
  uint32_t air = 0;
  // With the trace compiled out only the airtime sum below still needs this, so
  // the synthetic TX-FAIL/TX-BUSY rows stop paying for a CR register read.
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
    // Node-wide time on air. Kept here rather than in a Dispatcher because one
    // identity can only account for its own share of transmit, which is what
    // makes a per-identity radio watchdog unsound behind a shared radio.
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
  // hash from the FULL frame, before the raw capture is truncated
  e.hash_ok = frameHash(bytes, len, e.hash);
  if (!e.hash_ok) memset(e.hash, 0, sizeof(e.hash));
  e.raw_len = (uint8_t)(len > PKT_TRACE_RAW_CAP ? PKT_TRACE_RAW_CAP : len);
  if (e.raw_len > 0 && bytes != nullptr) memcpy(e.raw, bytes, e.raw_len); else e.raw_len = 0;
  e.seq = _pkt_seq + 1;   // written last; readers treat seq==0 / stale seq as invalid
  _pkt_seq = _pkt_seq + 1;
#else
  (void)bytes; (void)snr4; (void)rssi; (void)cr;
#endif
}

int SharedRadioCore::pktLogCopy(PktLogEntry* out, int max_entries, uint32_t after_seq) {
#if PKT_TRACE_ENTRIES
  // single-writer ring; a torn read can only affect the entry being overwritten,
  // which the seq check filters out
  uint32_t newest = _pkt_seq;
  uint32_t oldest = newest > PKT_TRACE_ENTRIES ? newest - PKT_TRACE_ENTRIES : 0;
  if (after_seq < oldest) after_seq = oldest;
  int n = 0;
  for (uint32_t s = after_seq + 1; s <= newest && n < max_entries; s++) {
    PktLogEntry e = _pkt_log[(s - 1) % PKT_TRACE_ENTRIES];
    if (e.seq != s) continue;   // overwritten mid-copy
    out[n++] = e;
  }
  return n;
#else
  (void)out; (void)max_entries; (void)after_seq;
  return 0;   // compiled out: no history, rather than stale or invented rows
#endif
}

bool SharedRadioCore::isSendCompleteFor(RadioPort* p) {
  if (_tx_owner != p) return true;   // not our send; report done so nobody hangs
  bool done = _real->isSendComplete();
  if (done) _tx_completed = true;
  return done;
}

void SharedRadioCore::onSendFinishedFor(RadioPort* p) {
  if (_tx_owner == p) {
    if (!_tx_completed) {
      // the dispatcher gave up (send expiry) before TX-done arrived
      pktLogAdd((int8_t)portIndex(p), nullptr, 0, 0, 0, PKT_FLAG_TX_FAIL);
    }
    _real->onSendFinished();
    _tx_owner = nullptr;
  }
}

void SharedRadioCore::applyRadioPolicy(bool recalibrate) {
  bool cad = false;
  int thresh = 0;          // 0 means "RSSI check off"
  for (int i = 0; i < _num_ports; i++) {
    if ((_active_mask & (1u << i)) == 0) continue;   // a silenced identity has no say
    if (_ports[i]->wantsCAD()) cad = true;
    int t = _ports[i]->wantedThreshold();
    // a lower non-zero threshold defers to weaker signals, so it is the more
    // cautious of the two; any identity asking for the check turns it on
    if (t != 0 && (thresh == 0 || t < thresh)) thresh = t;
  }

  if (_cad_applied != (int8_t)cad) {
    _cad_applied = (int8_t)cad;
    _real->setCADEnabled(cad);
  }

  // Recalibration resets the noise-floor sampling window. Three dispatchers
  // asking every 2s would restart it three times as often as the stock design
  // intends, so it is rate-limited back to the original cadence — except when
  // the threshold itself changed, which must take effect immediately.
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
    // Frames already queued arrived while this port was silent — mark them
    // consumed so it doesn't wake up to a backlog of packets it never heard.
    for (uint8_t k = 0; k < _rx_count; k++) _rx[(_rx_head + k) % RX_SLOTS].consumed |= bit;
    _active_mask |= bit;
  } else {
    _active_mask &= ~bit;
    // never wait on a silenced port, or its frames could never retire
    for (uint8_t k = 0; k < _rx_count; k++) _rx[(_rx_head + k) % RX_SLOTS].consumed |= bit;
    retireConsumedFrames();
    if (_tx_owner != nullptr && portIndex(_tx_owner) == idx) {
      _real->onSendFinished();    // don't strand the transmitter
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
  _real->begin();   // attaches the radio ISR (RX-done / TX-done) among other init
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
  // Not just "is the channel busy": every reason this identity may not key up
  // right now, since the dispatcher's back-off is the only lever we have from
  // below mesh::Radio. See SharedRadioCore::portMustWait().
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

// These two record what this identity's prefs want; the core reconciles the
// three requests into one setting for the shared radio.
void RadioPort::triggerNoiseFloorCalibrate(int threshold) {
  _thresh_want = threshold;
  if (_core) _core->applyRadioPolicy(true);
}

void RadioPort::setCADEnabled(bool enable) {
  _cad_want = enable;
  if (_core) _core->applyRadioPolicy(false);
}

void RadioPort::resetAGC() {
  // resetAGC() warm-sleeps the radio; another port's periodic AGC timer must
  // not do that while a transmit is in flight.
  if (_core && !_core->txInFlight()) _core->real()->resetAGC();
}
