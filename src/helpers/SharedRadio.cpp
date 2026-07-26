#include "SharedRadio.h"
#include <Arduino.h>

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
    enqueueRx(f.buf, f.len, 12.0f, -20.0f, f.from >= 0 ? (1u << f.from) : 0);
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
    enqueueRx(tmp, len, _real->getLastSNR(), _real->getLastRSSI(), 0);
    pktLogAdd(-1, tmp, len, (int8_t)(_real->getLastSNR() * 4), (int16_t)_real->getLastRSSI());
    checkRelayConfirmation(tmp, len);   // did someone relay something we sent?
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
                                uint32_t consumed_init) {
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
    p->setLastMetadata(f.snr, f.rssi);
    f.consumed |= bit;
    _port_rx[idx] = _port_rx[idx] + 1;
    _port_last_ms[idx] = millis();
    retireConsumedFrames();
    return len;
  }
  return 0;
}

bool SharedRadioCore::tryStartSend(RadioPort* p, const uint8_t* bytes, int len) {
  if (_tx_owner != nullptr) {
    // A sibling identity holds the transmitter. The dispatcher just backs off
    // and retries, so this used to be completely invisible — count it (overall
    // and per identity) and log a trace event so contention between the roles
    // on this board can actually be seen.
    _tx_contention = _tx_contention + 1;
    int idx = portIndex(p);
    if (idx >= 0 && idx < MAX_PORTS) _tx_contention_port[idx] = _tx_contention_port[idx] + 1;
    pktLogAdd((int8_t)idx, nullptr, 0, 0, 0, PKT_FLAG_TX_BUSY, (int16_t)portIndex(_tx_owner));
    return false;   // transmitter busy -> caller (Dispatcher) will drop & retry
  }
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
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0, PKT_FLAG_TX_FAIL, -1);
    return false;
  }
  {
    _tx_owner = p;
    _tx_started_ms = millis();
    _tx_completed = false;
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0);
    int pidx = portIndex(p);
    if (pidx >= 0 && pidx < MAX_PORTS) {
      _port_tx[pidx] = _port_tx[pidx] + 1;    // per-identity liveness
      _port_last_ms[pidx] = millis();
    }
    // only floods get relayed onward, so only they can be confirmed this way
    uint8_t route = len > 0 ? (bytes[0] & 0x03) : 0xFF;
    if (route == 0 || route == 1) noteFloodSent(pidx);

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

void SharedRadioCore::pktLogAdd(int8_t dir, const uint8_t* bytes, int len, int8_t snr4, int16_t rssi, uint8_t flag, int16_t aux) {
  if (flag == PKT_FLAG_OK) {
    if (dir < 0) _rx_total = _rx_total + 1; else _tx_total = _tx_total + 1;
  }
  PktLogEntry& e = _pkt_log[_pkt_seq % PKT_LOG_SIZE];
  e.t_ms = millis();
  e.dir = dir;
  e.flag = flag;
  e.hdr = (len > 0 && bytes != nullptr) ? bytes[0] : 0;
  e.len = (uint8_t)(len > 255 ? 255 : len);
  e.snr4 = snr4; e.rssi = rssi; e.aux = aux;
  e.raw_len = (uint8_t)(len > PKT_RAW_CAP ? PKT_RAW_CAP : len);
  if (e.raw_len > 0 && bytes != nullptr) memcpy(e.raw, bytes, e.raw_len); else e.raw_len = 0;
  e.seq = _pkt_seq + 1;   // written last; readers treat seq==0 / stale seq as invalid
  _pkt_seq = _pkt_seq + 1;
}

int SharedRadioCore::pktLogCopy(PktLogEntry* out, int max_entries, uint32_t after_seq) {
  // single-writer ring; a torn read can only affect the entry being overwritten,
  // which the seq check filters out
  uint32_t newest = _pkt_seq;
  uint32_t oldest = newest > PKT_LOG_SIZE ? newest - PKT_LOG_SIZE : 0;
  if (after_seq < oldest) after_seq = oldest;
  int n = 0;
  for (uint32_t s = after_seq + 1; s <= newest && n < max_entries; s++) {
    PktLogEntry e = _pkt_log[(s - 1) % PKT_LOG_SIZE];
    if (e.seq != s) continue;   // overwritten mid-copy
    out[n++] = e;
  }
  return n;
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

// ---- relay confirmation -----------------------------------------------------

void SharedRadioCore::noteFloodSent(int port_idx) {
  if (port_idx < 0 || port_idx >= MAX_PORTS) return;
  _flood_sent[port_idx] = _flood_sent[port_idx] + 1;
  TxRecord& r = _tx_ring[(_tx_ring_head + _tx_ring_count) % TX_RING];
  r.t_ms = millis();
  r.port = (int8_t)port_idx;
  r.confirmed = false;
  if (_tx_ring_count < TX_RING) _tx_ring_count++;
  else _tx_ring_head = (_tx_ring_head + 1) % TX_RING;   // overwrite the oldest
}

// A received flood carrying one of our hashes in its path means a neighbour
// heard us and relayed it onward.
void SharedRadioCore::checkRelayConfirmation(const uint8_t* frame, int len) {
  if (_port_hash_set == 0 || frame == nullptr || len < 2) return;

  uint8_t route = frame[0] & 0x03;
  int o = 1;
  if (route == 0 || route == 3) o += 4;          // transport codes
  if (o >= len) return;
  uint8_t pl = frame[o++];
  uint8_t hops = pl & 63;
  uint8_t sz = (pl >> 6) + 1;                    // 1..4 byte hashes
  if (hops == 0 || o + hops * sz > len) return;

  for (uint8_t h = 0; h < hops; h++) {
    const uint8_t* hop = &frame[o + h * sz];
    for (int p = 0; p < _num_ports; p++) {
      if ((_port_hash_set & (1u << p)) == 0) continue;
      if (memcmp(hop, _port_hash[p], sz) != 0) continue;

      // credit the most recent unconfirmed transmit from that port
      uint32_t now = millis();
      for (int k = _tx_ring_count - 1; k >= 0; k--) {
        TxRecord& r = _tx_ring[(_tx_ring_head + k) % TX_RING];
        if (r.port != p || r.confirmed) continue;
        if ((uint32_t)(now - r.t_ms) > CONFIRM_WINDOW_MS) break;   // older ones are older still
        r.confirmed = true;
        _flood_confirmed[p] = _flood_confirmed[p] + 1;
        _confirm_width[sz - 1] = _confirm_width[sz - 1] + 1;
        return;
      }
      return;   // our hash is present but no recent send of ours to credit
    }
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
  if (!_core) return false;
  // Busy if a sibling owns the transmitter, or the real channel is active.
  if (_core->txBusyForOthers(this)) return true;
  return _core->real()->isReceiving();
}

uint32_t RadioPort::getEstAirtimeFor(int len_bytes) {
  return _core ? _core->real()->getEstAirtimeFor(len_bytes) : 0;
}

float RadioPort::packetScore(float snr, int packet_len) {
  return _core ? _core->real()->packetScore(snr, packet_len) : 0;
}

int RadioPort::getNoiseFloor() const {
  return _core ? _core->real()->getNoiseFloor() : 0;
}

void RadioPort::triggerNoiseFloorCalibrate(int threshold) {
  if (_core) _core->real()->triggerNoiseFloorCalibrate(threshold);
}

void RadioPort::setCADEnabled(bool enable) {
  if (_core) _core->real()->setCADEnabled(enable);
}

void RadioPort::resetAGC() {
  // resetAGC() warm-sleeps the radio; another port's periodic AGC timer must
  // not do that while a transmit is in flight.
  if (_core && !_core->txInFlight()) _core->real()->resetAGC();
}
