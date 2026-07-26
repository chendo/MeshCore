#pragma once

// SharedRadio: lets several mesh::Mesh instances (separate identities) share a
// single physical LoRa radio on one board.
//
// The real radio driver owns one RX/TX state machine, so two Dispatchers can't
// both drive it. Instead the SharedRadioCore owns the real driver, and each
// mesh is handed its own RadioPort (a mesh::Radio) that funnels through the
// core:
//
//   * RX fan-out: the core pumps the real radio once per super-loop, buffers
//     each received frame with an SNR/RSSI snapshot, and delivers that same
//     frame to every port exactly once. So both identities see all traffic and
//     each decides independently whether it's addressed to them / should be
//     forwarded.
//
//   * TX serialization: only one port may transmit at a time. The first to
//     start a send becomes the owner; while it holds the transmitter, other
//     ports' startSendRaw() returns false (their Dispatcher drops+retries) AND
//     their isReceiving() returns true, so the existing CAD/back-off logic
//     defers them cleanly. Just before a send the core applies that port's
//     TX power (per-persona power offset / jitter).
//
// Everything is cooperative and single-threaded, driven from the main loop;
// there are no interrupts mutating shared state, so no locking is needed.
//
// Super-loop order MUST be:  meshA.loop(); meshB.loop(); shared_core.pump();
// so both ports consume the current frame before the next one is fetched.

#include <Mesh.h>
#include <MeshCore.h>

class SharedRadioCore;

class RadioPort : public mesh::Radio {
public:
  RadioPort() : _core(nullptr), _last_snr(0), _last_rssi(0), _tx_power_dbm(0) {}
  void attach(SharedRadioCore* core) { _core = core; }

  // per-persona TX power applied by the core right before this port transmits
  void setPortTxPower(int8_t dbm) { _tx_power_dbm = dbm; }
  int8_t portTxPower() const { return _tx_power_dbm; }

  // mesh::Radio interface
  void begin() override;   // forwards to the real driver's begin() (once) — attaches the DIO1 ISR
  int recvRaw(uint8_t* bytes, int sz) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override { return true; }   // logically always listening
  bool isReceiving() override;

  uint32_t getEstAirtimeFor(int len_bytes) override;
  float packetScore(float snr, int packet_len) override;
  int getNoiseFloor() const override;
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override;
  void resetAGC() override;
  void loop() override {}   // real radio is pumped centrally via SharedRadioCore::pump()

  float getLastRSSI() const override { return _last_rssi; }
  float getLastSNR() const override { return _last_snr; }

  // called by the core when delivering a buffered frame to this port
  void setLastMetadata(float snr, float rssi) { _last_snr = snr; _last_rssi = rssi; }

private:
  SharedRadioCore* _core;
  float _last_snr, _last_rssi;
  int8_t _tx_power_dbm;
};

// Optional: something that can set the real radio's TX power (RadioLibWrapper
// implements setTxPower). Kept as a tiny interface so SharedRadio doesn't have
// to depend on the concrete wrapper type.
class TxPowerControl {
public:
  virtual void applyTxPower(int8_t dbm) = 0;
};

// One row of the radio packet trace kept by SharedRadioCore (for diagnostics /
// the web debug panel). dir: -1 = received, >= 0 = transmitted by that port.
// flag: 0 = ok, 1 = RX decode/CRC failure, 2 = TX never completed (timed out).
#define PKT_RAW_CAP 200
#define PKT_FLAG_OK      0
#define PKT_FLAG_RX_ERR  1
#define PKT_FLAG_TX_FAIL 2
#define PKT_FLAG_TX_BUSY 3   // send deferred: a sibling identity held the radio
struct PktLogEntry {
  uint32_t seq;
  uint32_t t_ms;
  int8_t   dir;
  uint8_t  flag;
  uint8_t  hdr;      // raw packet header byte (route/type bits)
  uint8_t  len;      // full over-the-air length
  int8_t   snr4;     // SNR * 4 (RX only)
  int16_t  rssi;     // RX only
  int16_t  aux;      // flag-specific: RX_ERR -> RadioLib error code
  uint8_t  raw_len;  // bytes captured in raw[] (<= len, capped at PKT_RAW_CAP)
  uint8_t  raw[PKT_RAW_CAP];
};

class SharedRadioCore {
public:
  static const int MAX_PORTS = 8;
  static const int PKT_LOG_SIZE = 48;

  explicit SharedRadioCore(mesh::Radio& real) : _real(&real), _num_ports(0),
      _rx_len(0), _rx_snr(0), _rx_rssi(0), _consumed_mask(0),
      _tx_owner(nullptr), _pwr_ctl(nullptr), _applied_pwr(0x7F) {}

  void setTxPowerControl(TxPowerControl* ctl) { _pwr_ctl = ctl; }

  // Ports may be registered inactive and switched on/off at runtime, so an
  // identity can be started or silenced without a reboot. Indices stay stable
  // (the packet trace and TX-owner bookkeeping reference them), and inactive
  // ports are excluded from the delivery mask so a frame is never held waiting
  // for a port that isn't listening.
  int addPort(RadioPort& port, bool active = true) {   // returns port index
    int idx = _num_ports;
    _ports[_num_ports++] = &port;
    port.attach(this);
    setPortActive(idx, active);
    return idx;
  }
  void setPortActive(int idx, bool active);
  bool portActive(int idx) const;

  // Pump the real radio once: if the current frame is fully delivered, fetch
  // the next one. Call AFTER every mesh has had its loop() this cycle.
  void pump();

  // --- called by RadioPort ---
  void beginRealOnce();
  int  takeFrame(RadioPort* p, uint8_t* dst, int sz);
  bool tryStartSend(RadioPort* p, const uint8_t* bytes, int len);
  bool isSendCompleteFor(RadioPort* p);
  void onSendFinishedFor(RadioPort* p);
  bool txBusyForOthers(RadioPort* p) const { return _tx_owner != nullptr && _tx_owner != p; }
  bool txInFlight() const { return _tx_owner != nullptr; }

  mesh::Radio* real() { return _real; }

  // --- packet trace (thread-safe copy for the web debug panel) ---
  uint32_t pktLogSeq() const { return _pkt_seq; }
  uint32_t rxTotal() const { return _rx_total; }
  uint32_t txTotal() const { return _tx_total; }
  // sends refused because a sibling identity held the transmitter — the cost
  // of sharing one radio, invisible until now because the dispatcher just
  // silently backs off and retries
  uint32_t txContention() const { return _tx_contention; }
  // transmits force-released by the watchdog below (see pump())
  uint32_t txStuck() const { return _tx_stuck; }
  // sends the radio itself refused (RadioLib error) — a real TX failure
  uint32_t txRefused() const { return _tx_refused; }
  // times the radio was re-initialised after going silent
  uint32_t radioRecoveries() const { return _radio_recoveries; }
  uint32_t msSinceLastRx() const { return _last_rx_ms ? (uint32_t)(millis() - _last_rx_ms) : 0; }
  // how the composition puts a wedged transceiver back together
  void setRadioReinit(void (*fn)()) { _reinit_fn = fn; }

  // ---- relay confirmation --------------------------------------------------
  // Did anyone actually hear us? When another node relays a flood we sent, it
  // appends its own hash and re-transmits — so a received packet carrying OUR
  // hash in its path is proof that a neighbour received our transmission and
  // passed it on. Give the arbiter each port's public key so it can spot that.
  //
  // Matches are counted separately by hash width because they are not equally
  // trustworthy: a 1-byte hash collides roughly 1 in 256 per hop, so with
  // several hops per packet a fair number of "confirmations" are coincidence.
  // A 2-byte match is ~1 in 65536 — effectively certain.
  void setPortIdentity(int idx, const uint8_t* pub_key) {
    if (idx < 0 || idx >= MAX_PORTS || pub_key == nullptr) return;
    memcpy(_port_hash[idx], pub_key, 4);
    _port_hash_set |= (1u << idx);
  }
  uint32_t floodsSent(int idx) const { return (idx >= 0 && idx < MAX_PORTS) ? _flood_sent[idx] : 0; }
  uint32_t floodsConfirmed(int idx) const { return (idx >= 0 && idx < MAX_PORTS) ? _flood_confirmed[idx] : 0; }
  uint32_t confirmsByWidth(int bytes) const {   // 1..4
    return (bytes >= 1 && bytes <= 4) ? _confirm_width[bytes - 1] : 0;
  }
  uint32_t txContentionFor(int idx) const {
    return (idx >= 0 && idx < MAX_PORTS) ? _tx_contention_port[idx] : 0;
  }
  // copy entries with seq > after_seq into out (oldest first); returns count
  int pktLogCopy(PktLogEntry* out, int max_entries, uint32_t after_seq);
  const char* portName(int idx) const { return (idx >= 0 && idx < _num_ports) ? _port_names[idx] : "?"; }
  void setPortName(int idx, const char* name) { if (idx >= 0 && idx < MAX_PORTS) _port_names[idx] = name; }
  // lets pump() notice RX decode/CRC failures inside the real driver (which
  // reports them only via a counter) and log them as trace events; the code
  // accessor supplies the RadioLib error code (CRC mismatch vs header damage)
  void setRxErrorCounter(uint32_t (*fn)(), int16_t (*code_fn)() = nullptr,
                         const uint8_t* (*payload_fn)() = nullptr, uint8_t (*len_fn)() = nullptr) {
    _rx_err_fn = fn; _rx_err_code_fn = code_fn;
    _rx_err_payload_fn = payload_fn; _rx_err_len_fn = len_fn;
    _rx_err_seen = fn ? fn() : 0;
  }

  // TX loopback: identities on this board share one antenna and the radio is
  // half-duplex, so nothing any of them transmits is ever heard by the others
  // — the companion cannot see its own room, the repeater cannot relay for
  // them, etc. With loopback on, every transmitted frame is also delivered to
  // the sibling ports (not the sender) as if received, which is what a second
  // physical node in the same room would experience.
  void setLoopback(bool on) { _loopback = on; }
  bool loopback() const { return _loopback; }

private:
  int portIndex(RadioPort* p) const {
    for (int i = 0; i < _num_ports; i++) if (_ports[i] == p) return i;
    return -1;
  }
  uint32_t allPortsMask() const { return _active_mask; }   // only listening ports must consume a frame

  mesh::Radio* _real;
  RadioPort* _ports[MAX_PORTS];
  int _num_ports;

  uint8_t  _rx_buf[MAX_TRANS_UNIT];
  int      _rx_len;
  float    _rx_snr, _rx_rssi;
  uint32_t _consumed_mask;   // bit i set once port i has taken the current frame

  RadioPort* _tx_owner;
  TxPowerControl* _pwr_ctl;
  int8_t   _applied_pwr;     // last power applied to the real radio (avoid redundant writes)
  bool     _real_begun = false;   // real driver's begin() must run exactly once

  void pktLogAdd(int8_t dir, const uint8_t* bytes, int len, int8_t snr4, int16_t rssi, uint8_t flag = 0, int16_t aux = 0);
  uint32_t (*_rx_err_fn)() = nullptr;
  int16_t (*_rx_err_code_fn)() = nullptr;
  const uint8_t* (*_rx_err_payload_fn)() = nullptr;
  uint8_t (*_rx_err_len_fn)() = nullptr;
  uint32_t _rx_err_seen = 0;

  // loopback queue (frames sent by one port, pending delivery to the others)
  static const int LB_SLOTS = 4;
  struct LbFrame { uint8_t buf[MAX_TRANS_UNIT]; uint8_t len; int8_t from; };
  LbFrame _lb[LB_SLOTS];
  uint8_t _lb_head = 0, _lb_count = 0;
  bool _loopback = true;
  uint32_t _active_mask = 0;   // which ports are currently listening
  bool _tx_completed = false;   // did the current owner's send reach TX-done?
  PktLogEntry _pkt_log[PKT_LOG_SIZE];
  volatile uint32_t _pkt_seq = 0;   // total packets ever logged; ring index = seq % SIZE
  volatile uint32_t _rx_total = 0, _tx_total = 0;
  volatile uint32_t _tx_contention = 0;
  volatile uint32_t _tx_stuck = 0;
  volatile uint32_t _tx_refused = 0;
  volatile uint32_t _radio_recoveries = 0;
  uint32_t _tx_started_ms = 0;
  uint32_t _last_rx_ms = 0;
  void (*_reinit_fn)() = nullptr;

  // relay confirmation bookkeeping
  static const int TX_RING = 16;
  static const uint32_t CONFIRM_WINDOW_MS = 30000;   // how long a relay may take
  struct TxRecord { uint32_t t_ms; int8_t port; bool confirmed; };
  TxRecord _tx_ring[TX_RING];
  uint8_t  _tx_ring_head = 0, _tx_ring_count = 0;
  uint8_t  _port_hash[MAX_PORTS][4];
  uint32_t _port_hash_set = 0;
  volatile uint32_t _flood_sent[MAX_PORTS] = {0};
  volatile uint32_t _flood_confirmed[MAX_PORTS] = {0};
  volatile uint32_t _confirm_width[4] = {0};
  void noteFloodSent(int port_idx);
  void checkRelayConfirmation(const uint8_t* frame, int len);
  // How long a silent radio is tolerated before it is assumed wedged. Long
  // enough that a genuinely quiet band never trips it; short enough that the
  // node is not off air for hours.
  static const uint32_t RX_SILENCE_LIMIT_MS = 900000;   // 15 minutes
  // Longest a port may hold the transmitter before the arbiter takes it back.
  // Well beyond any legal LoRa airtime (a 255-byte frame at SF12/BW125 is
  // ~9s); this is a deadlock breaker, not a timing parameter.
  static const uint32_t TX_HOLD_LIMIT_MS = 15000;
  volatile uint32_t _tx_contention_port[MAX_PORTS] = {0};
  const char* _port_names[MAX_PORTS] = { "?", "?", "?", "?", "?", "?", "?", "?" };
};
