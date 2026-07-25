#include "SharedRadio.h"
#include <Arduino.h>

// ------------------------------------------------------------------ core

void SharedRadioCore::pump() {
  // While a port owns the transmitter, leave the real radio alone: recvRaw()
  // re-arms RX (startReceive()) whenever state != STATE_RX, which would abort
  // an in-flight transmit and its TX-done interrupt would never arrive.
  if (_tx_owner != nullptr) {
    return;
  }

  _real->loop();   // ports' loop() is a stub; the real driver is serviced here

  // Hold the current frame until every port has consumed it, then fetch next.
  if (_rx_len > 0 && _consumed_mask != allPortsMask()) {
    return;   // still delivering the buffered frame this cycle
  }

  // Deliver a locally-transmitted frame to the sibling identities before
  // pulling the next one off the air (see setLoopback()).
  if (_lb_count > 0) {
    LbFrame& f = _lb[_lb_head];
    memcpy(_rx_buf, f.buf, f.len);
    _rx_len = f.len;
    _rx_snr = 12.0f;      // synthetic: same board, effectively perfect link
    _rx_rssi = -20.0f;
    _consumed_mask = (f.from >= 0) ? (1u << f.from) : 0;   // sender doesn't hear itself
    _lb_head = (_lb_head + 1) % LB_SLOTS;
    _lb_count--;
    return;
  }

  uint8_t tmp[MAX_TRANS_UNIT];
  int len = _real->recvRaw(tmp, sizeof(tmp));   // also re-arms real RX

  // surface RX decode/CRC failures (driver only counts them) as trace events
  if (_rx_err_fn != nullptr) {
    uint32_t errs = _rx_err_fn();
    int16_t code = _rx_err_code_fn ? _rx_err_code_fn() : 0;
    while (_rx_err_seen < errs) {
      _rx_err_seen++;
      pktLogAdd(-1, nullptr, 0, (int8_t)(_real->getLastSNR() * 4), (int16_t)_real->getLastRSSI(),
                PKT_FLAG_RX_ERR, code);
    }
  }

  if (len > 0) {
    memcpy(_rx_buf, tmp, len);
    _rx_len = len;
    _rx_snr = _real->getLastSNR();
    _rx_rssi = _real->getLastRSSI();
    _consumed_mask = 0;
    pktLogAdd(-1, tmp, len, (int8_t)(_rx_snr * 4), (int16_t)_rx_rssi);
  } else {
    _rx_len = 0;   // nothing pending
  }
}

int SharedRadioCore::takeFrame(RadioPort* p, uint8_t* dst, int sz) {
  if (_rx_len <= 0) return 0;
  int idx = portIndex(p);
  if (idx < 0) return 0;
  uint32_t bit = (1u << idx);
  if (_consumed_mask & bit) return 0;   // this port already got it

  int len = _rx_len;
  if (len > sz) len = sz;
  memcpy(dst, _rx_buf, len);
  p->setLastMetadata(_rx_snr, _rx_rssi);
  _consumed_mask |= bit;
  return len;
}

bool SharedRadioCore::tryStartSend(RadioPort* p, const uint8_t* bytes, int len) {
  if (_tx_owner != nullptr) {
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
  if (ok) {
    _tx_owner = p;
    _tx_completed = false;
    pktLogAdd((int8_t)portIndex(p), bytes, len, 0, 0);

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
  e.hdr = len > 0 ? bytes[0] : 0;
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
