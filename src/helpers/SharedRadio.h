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
//   * DUTY-CYCLE BUDGET: pooled here, because the duty cycle is a property of
//     the radio and not of an identity. Dispatcher keeps tx_budget_ms per Mesh
//     instance, so N identities behind one antenna is N budgets for one
//     transmitter — three slots each believing they hold 50% duty would put
//     the node on air at 150%. See setDutyCycle().
//
//   * PRIORITY: ports may be ranked, so routed repeater traffic is not left
//     behind a chatty chat identity. Upstream has priority WITHIN a Dispatcher
//     (ACTION_RETRANSMIT_DELAYED(0, d)); this is the missing cross-identity
//     equivalent.
//
// Everything is cooperative and single-threaded, driven from the main loop;
// there are no interrupts mutating shared state, so no locking is needed.
//
// Super-loop order MUST be:  meshA.loop(); meshB.loop(); shared_core.pump();
// so both ports consume the current frame before the next one is fetched.

#include <Arduino.h>   // millis(): reaches us transitively on ESP32, not on nRF52
#include "MeshObserver.h"
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

  // what this identity's prefs asked for; the core decides what the shared
  // radio actually does (see applyRadioPolicy)
  bool wantsCAD() const { return _cad_want; }
  int  wantedThreshold() const { return _thresh_want; }

private:
  SharedRadioCore* _core;
  float _last_snr, _last_rssi;
  int8_t _tx_power_dbm;
  bool _cad_want = false;
  int  _thresh_want = 0;
};

// Optional: something that can set the real radio's TX power (RadioLibWrapper
// implements setTxPower). Kept as a tiny interface so SharedRadio doesn't have
// to depend on the concrete wrapper type.
class TxPowerControl {
public:
  virtual void applyTxPower(int8_t dbm) = 0;
};

// ---- packet trace sizing --------------------------------------------------
// PKT_TRACE_ENTRIES is the ring depth. 0 or undefined compiles the buffer, the
// writer and the reader out entirely -- not a zero-length array that still
// costs. Default off: at the full 48 x 200 the trace is 10.9 KB of RAM carried
// whether or not anyone is debugging, which is most of what sharing a radio
// appears to cost.
//
// Deliberately NOT gated on MESH_DEBUG. That flag is the project's serial
// logging switch; tying a 10.9 KB buffer to it means you cannot have the trace
// without a flood of serial output, or serial output without the buffer.
//
// The raw capture has its own flag because the two dials answer different
// questions and the cost is their product. "Did packets flow, and when" wants
// depth and needs only the header; "what exactly was in that frame" wants the
// bytes and only for a handful of frames. One flag would force you to trade
// history for detail at a fixed ratio.
#ifndef PKT_TRACE_ENTRIES
  #define PKT_TRACE_ENTRIES 0
#endif
#ifndef PKT_TRACE_RAW_CAP
  #define PKT_TRACE_RAW_CAP 200
#endif

// One row of the radio packet trace kept by SharedRadioCore, for diagnostics.
// dir: -1 = received, >= 0 = transmitted by that port.
// flag: 0 = ok, 1 = RX decode/CRC failure, 2 = TX never completed (timed out).
#define PKT_FLAG_OK      0
#define PKT_FLAG_RX_ERR  1
#define PKT_FLAG_TX_FAIL 2
#define PKT_FLAG_TX_BUSY 3   // send deferred: a sibling identity held the radio
// RadioLib's RADIOLIB_ERR_CRC_MISMATCH, as it lands in aux for an RX_ERR entry.
// It is the one receive failure whose LoRa header still decoded — the length
// and coding rate are trustworthy and only the payload was mangled. Every other
// failure means the header itself was lost, so nothing read out of it is ours
// to report.
#define PKT_RX_ERR_CRC   (-7)

// Why a transmission did not happen. A node that cannot get a word in edgeways
// is otherwise indistinguishable from one on a quiet band: both just sit there.
// TXWAIT_RX_PACKET / _RSSI / _CAD are the three separate conditions hiding
// behind the driver's single isReceiving() bool, and they mean quite different
// things — a neighbour mid-packet, an interference threshold set too tight, and
// hardware CAD respectively.
#define TXWAIT_BUDGET      0   // pooled duty-cycle budget exhausted
#define TXWAIT_SIBLING     1   // another identity on this board held the transmitter
#define TXWAIT_PRIORITY    2   // yielded to a higher-priority identity
#define TXWAIT_RX_PACKET   3   // channel busy: preamble/header already detected
#define TXWAIT_RSSI        4   // channel busy: RSSI above noise floor + threshold
#define TXWAIT_CAD         5   // channel busy: hardware CAD said occupied
#define TXWAIT_RADIO       6   // the driver itself refused the send
#define TXWAIT_FORCED      7   // sent anyway, over a live higher-priority claim
#define TXWAIT_NUM         8

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
  // MeshCore's own packet hash (Packet::calculatePacketHash), first 4 bytes.
  // It is taken over the payload and NOT the path, which is the whole point:
  // every copy of one flood carries the same hash however many hops it has
  // travelled, so repeats of a packet being forwarded around the mesh can be
  // told apart from genuinely new traffic at a glance.
  //
  // Computed here rather than by the reader because raw[] is truncated at
  // PKT_TRACE_RAW_CAP: a long packet's payload is no longer all present downstream,
  // so a hash taken there would silently be wrong.
  uint8_t  hash[4];
  bool     hash_ok;  // false when the frame could not be parsed into a payload
  // Time on air in ms, from the radio driver's own airtime sum rather than a
  // reimplemented formula — it already accounts for preamble, CRC, explicit
  // header and the low-data-rate optimisation. A transmission is priced at the
  // settings we sent it with; a receive is priced at the CR out of ITS header
  // (see cr below), so a neighbour on a different coding rate is costed at what
  // it really spent on the channel rather than at what we would have spent.
  // SF and BW have to match ours or the packet would not have decoded, and the
  // preamble length is the one term LoRa does not transmit — ours is assumed.
  uint16_t air_ms;
  // Coding rate as the 4/x denominator (5..8); 0 when not known. For a received
  // frame this is the CR the SENDER used, read out of the explicit LoRa header
  // the radio just decoded — not our own setting — so a neighbour running a
  // different CR shows up as such instead of being assumed away. For a
  // transmission it is the CR the shared radio is configured with.
  uint8_t  cr;
  uint8_t  raw_len;  // bytes captured in raw[] (<= len, capped at PKT_TRACE_RAW_CAP)
  uint8_t  raw[PKT_TRACE_RAW_CAP];
};

class SharedRadioCore {
public:
  static const int MAX_PORTS = 8;
  // Ring depth; 0 when the trace is compiled out, which every reader below
  // degrades to rather than failing to build.
  static const int PKT_LOG_SIZE = PKT_TRACE_ENTRIES;
  static const int PKT_LOG_RAW_CAP = PKT_TRACE_RAW_CAP;
  static bool pktTraceEnabled() { return PKT_TRACE_ENTRIES != 0; }
  // Received frames are QUEUED rather than held one at a time. The radio is
  // drained the moment a packet lands; identities consume from the queue at
  // their own pace. Previously the next packet could not be fetched until every
  // identity had taken the current one — and a dispatcher waiting on its own
  // transmit skips its receive step entirely, so one busy identity stalled
  // reception for all of them and packets were lost in the radio's FIFO.
  static const int RX_SLOTS = 6;

  explicit SharedRadioCore(mesh::Radio& real) : _real(&real), _num_ports(0),
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

  // ---- pooled duty-cycle budget --------------------------------------------
  // factor is Dispatcher's airtime budget factor: duty = 1/(1+factor), so 1.0
  // is 50%. Defaults to that, matching Dispatcher::getAirtimeBudgetFactor(), so
  // a composition that forgets to call this is capped rather than uncapped.
  //
  // The pool is charged the ESTIMATED airtime of a frame before that frame
  // reaches the air, not the measured airtime afterwards: a send that is later
  // force-released by the TX watchdog still used the channel, and a budget that
  // only debits on clean completion would not have noticed.
  void setDutyCycle(float airtime_budget_factor, uint32_t window_ms = 3600000);
  uint32_t txBudgetMs();                 // refills first, so this is the live figure
  uint32_t txBudgetMaxMs() const { return _duty_max_ms; }
  // Airtime the pool has been charged since boot. Never exceeds what the duty
  // cycle permits over the elapsed window; the invariant the tests pin down.
  uint32_t txChargedMs() const { return _tx_charged_ms; }

  // ---- cross-identity priority ---------------------------------------------
  // 0 = highest, default 1. Per port rather than "slot 0 always wins" because
  // the arbiter has no idea what a slot is, and a room server wants the same
  // standing as a repeater. A port that wanted the air and was denied it leaves
  // a CLAIM; lower-priority ports then report isReceiving() until the claim
  // expires or the claimant transmits, so their dispatchers back off cleanly.
  // Priority never REFUSES a send — a dispatcher that has burned through its
  // own CAD-busy timeout and force-sent is let through, which bounds how long
  // one identity can be held off at the 4 s that timeout already allows.
  void setPortPriority(int idx, uint8_t pri);
  uint8_t portPriority(int idx) const;

  // ---- deferral accounting --------------------------------------------------
  uint32_t txWaits(int reason) const {
    return (reason >= 0 && reason < TXWAIT_NUM) ? _tx_waits[reason] : 0;
  }
  // Splits the driver's isReceiving() into its three underlying conditions.
  // Supplied by the composition because doing it requires the concrete radio
  // wrapper, which this file deliberately knows nothing about. Returns one of
  // TXWAIT_RX_PACKET / TXWAIT_RSSI / TXWAIT_CAD.
  typedef int (*ChannelBusyProbe)();
  void setChannelBusyProbe(ChannelBusyProbe fn) { _busy_probe = fn; }

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
  // The whole of RadioPort::isReceiving(), so every reason a port is told to
  // wait is decided — and counted — in one place.
  bool portMustWait(RadioPort* p);

  mesh::Radio* real() { return _real; }

  // --- packet trace (single-writer ring; pktLogCopy() is safe from a reader) ---
#if PKT_TRACE_ENTRIES
  uint32_t pktLogSeq() const { return _pkt_seq; }
#else
  uint32_t pktLogSeq() const { return 0; }
#endif
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
  // Time on air, transmit + receive, as the REAL radio saw it — node-wide by
  // construction, since every identity's traffic passes through here. This is
  // the input the node's LoRa watchdog reasons from (helpers/LoraWatchdog.h):
  // a Dispatcher can only account for its own identity's share of transmit.
  // Receives are priced at the CR out of the sender's header, transmits at the
  // estimate the duty-cycle pool was charged.
  uint32_t airtimeMs() const { return _tx_air_ms + _rx_air_ms; }
  uint32_t txAirtimeMs() const { return _tx_air_ms; }
  uint32_t rxAirtimeMs() const { return _rx_air_ms; }
  uint32_t msSinceLastRx() const { return _last_rx_ms ? (uint32_t)(millis() - _last_rx_ms) : 0; }

  // COLLISION AVOIDANCE IS A PROPERTY OF THE RADIO, NOT OF AN IDENTITY.
  // Each stock mesh's dispatcher pushes its own cad_enabled and
  // interference_threshold prefs onto its radio every couple of seconds. With
  // one radio behind three of them that is three writers to one setting, and
  // whichever ran last decides — so an identity with CAD off silently disables
  // it for the others, and the RSSI threshold flaps between three values.
  // Transmitting on top of a packet already in the air corrupts both, which is
  // exactly the failure we are trying to remove, so the core takes the most
  // cautious setting any listening identity asked for.
  void applyRadioPolicy(bool recalibrate);
  bool cadEnabled() const { return _cad_applied > 0; }
  int  interferenceThreshold() const { return _thresh_applied; }
  // frames discarded because every identity was too slow to drain the queue
  uint32_t rxDropped() const { return _rx_dropped; }
  int rxQueued() const { return _rx_count; }
  // how the composition puts a wedged transceiver back together
  void setRadioReinit(void (*fn)()) { _reinit_fn = fn; }

  // ---- raw frame hook -------------------------------------------------------
  // Every frame the arbiter handles, exactly once, for anything that wants to
  // watch traffic without being an identity. The arbiter works in raw frames —
  // it sits below the Packet layer — so this hands over bytes and lets the
  // caller decide whether to parse them. A plain function pointer, so this file
  // stays free of whatever the composition plugs in (a bridge, an uplink).
  typedef void (*FrameHook)(const uint8_t* frame, int len, bool is_tx, float snr, float rssi);
  void setFrameHook(FrameHook fn) { _frame_hook = fn; }

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
    _obs.addSelfKey(pub_key);   // so it can tell "someone relayed US" from noise
  }
  uint32_t floodsSent(int idx) const { return _obs.floodsSent(idx); }
  uint32_t floodsConfirmed(int idx) const { return _obs.floodsConfirmed(idx); }
  uint32_t confirmsByWidth(int bytes) const { return _obs.confirmsByWidth(bytes); }

  // ---- observability --------------------------------------------------------
  // The peer table, hop and packet-type histograms live in MeshObserver, which
  // knows nothing about arbitration — a single-identity node uses the same code
  // with a plain receive hook. The arbiter just feeds it every frame it pumps.
  MeshObserver& observer() { return _obs; }
  const MeshObserver& observer() const { return _obs; }
  typedef MeshObserver::PeerEntry PeerEntry;
  int numPeers() const { return _obs.numPeers(); }
  const PeerEntry* peer(int i) const { return _obs.peer(i); }
  int confirmedPeerCount() const { return _obs.confirmedPeerCount(); }

  // ---- per-identity liveness -----------------------------------------------
  // Every identity funnels through a port, so the arbiter can report what each
  // one is actually doing — including the chat identities, which have no CLI
  // and therefore no stock stats of their own.
  int numPorts() const { return _num_ports; }
  uint32_t portRxCount(int idx) const { return (idx >= 0 && idx < MAX_PORTS) ? _port_rx[idx] : 0; }
  uint32_t portTxCount(int idx) const { return (idx >= 0 && idx < MAX_PORTS) ? _port_tx[idx] : 0; }
  // ms since this identity last sent or received anything (0 = never)
  uint32_t portIdleMs(int idx) const {
    if (idx < 0 || idx >= MAX_PORTS || _port_last_ms[idx] == 0) return 0;
    return (uint32_t)(millis() - _port_last_ms[idx]);
  }
  bool portEverActive(int idx) const { return idx >= 0 && idx < MAX_PORTS && _port_last_ms[idx] != 0; }
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

  // Coding rate bookkeeping for the packet trace. The CR in force is owned by
  // whoever configures the radio (the shared "set radio" path), so it is handed
  // in rather than guessed; the RX accessor reads the CR out of the LoRa header
  // of the frame just decoded, which is the sender's, not ours. Both are 4/x
  // denominators (5..8), 0 meaning unknown.
  void setCodingRate(uint8_t cr) { _cfg_cr = (cr >= 5 && cr <= 8) ? cr : 0; }
  uint8_t codingRate() const { return _cfg_cr; }
  void setRxCodingRateFn(uint8_t (*fn)()) { _rx_cr_fn = fn; }
  // airtime for a received frame priced at the CR its header carried, rather
  // than at ours (the driver's own sum can only speak for our settings)
  void setRxAirtimeFn(uint32_t (*fn)(int len_bytes, uint8_t cr)) { _rx_air_fn = fn; }

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

  struct RxFrame {
    uint8_t  buf[MAX_TRANS_UNIT];
    uint8_t  len;
    float    snr, rssi;
    uint32_t consumed;       // bit i set once port i has taken this frame
  };
  RxFrame  _rx[RX_SLOTS];
  uint8_t  _rx_head = 0, _rx_count = 0;
  volatile uint32_t _rx_dropped = 0;   // queue was full — the oldest was discarded
  bool enqueueRx(const uint8_t* bytes, int len, float snr, float rssi, uint32_t consumed_init);
  void retireConsumedFrames();

  int8_t   _cad_applied = -1;      // -1 = never applied
  int      _thresh_applied = 0;
  uint32_t _last_calib_ms = 0;
  static const uint32_t CALIB_MIN_INTERVAL_MS = 2000;   // stock per-mesh cadence

  // ---- pooled duty-cycle budget ----
  void     refillBudget();
  void     noteWait(int reason) { if (reason >= 0 && reason < TXWAIT_NUM) _tx_waits[reason]++; }
  float    _duty = 0.5f;                    // fraction of the window we may transmit for
  uint32_t _duty_window_ms = 3600000;
  uint32_t _duty_max_ms = 1800000;
  uint32_t _budget_ms = 1800000;
  uint32_t _budget_last_ms = 0;
  uint32_t _tx_charged_ms = 0;
  // Below this the pool stops volunteering for new sends, mirroring
  // Dispatcher's MIN_TX_BUDGET_RESERVE_MS so the two agree on what "nearly out"
  // means. The per-frame test in tryStartSend() is the hard cap; this is only
  // the low-water mark that makes ports back off before they get there.
  static const uint32_t BUDGET_RESERVE_MS = 100;
  volatile uint32_t _tx_waits[TXWAIT_NUM] = {0};
  ChannelBusyProbe _busy_probe = nullptr;

  // ---- cross-identity priority ----
  bool     higherPriorityClaimed(int idx) const;
  uint8_t  _port_pri[MAX_PORTS] = { 1, 1, 1, 1, 1, 1, 1, 1 };
  uint32_t _claim_ms[MAX_PORTS] = {0};      // when this port last wanted the air and was denied
  // Long enough for a denied port's next attempt to land (its dispatcher retries
  // every getCADFailRetryDelay(), 200 ms), short enough that a claimant which
  // goes quiet cannot hold the others off for any noticeable time.
  static const uint32_t CLAIM_TTL_MS = 1000;

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
  uint8_t (*_rx_cr_fn)() = nullptr;   // CR of the frame the radio just decoded
  uint32_t (*_rx_air_fn)(int, uint8_t) = nullptr;   // airtime at that CR
  uint8_t _cfg_cr = 0;                // CR the shared radio transmits with

  // loopback queue (frames sent by one port, pending delivery to the others)
  static const int LB_SLOTS = 4;
  struct LbFrame { uint8_t buf[MAX_TRANS_UNIT]; uint8_t len; int8_t from; };
  LbFrame _lb[LB_SLOTS];
  uint8_t _lb_head = 0, _lb_count = 0;
  bool _loopback = true;
  uint32_t _active_mask = 0;   // which ports are currently listening
  bool _tx_completed = false;   // did the current owner's send reach TX-done?
#if PKT_TRACE_ENTRIES
  PktLogEntry _pkt_log[PKT_TRACE_ENTRIES];
  volatile uint32_t _pkt_seq = 0;   // total packets ever logged; ring index = seq % SIZE
#endif
  volatile uint32_t _rx_total = 0, _tx_total = 0;
  volatile uint32_t _tx_contention = 0;
  volatile uint32_t _tx_stuck = 0;
  volatile uint32_t _tx_refused = 0;
  volatile uint32_t _radio_recoveries = 0;
  volatile uint32_t _tx_air_ms = 0, _rx_air_ms = 0;
  uint32_t _tx_started_ms = 0;
  uint32_t _last_rx_ms = 0;
  void (*_reinit_fn)() = nullptr;
  FrameHook _frame_hook = nullptr;

  // relay confirmation bookkeeping
  uint8_t  _port_hash[MAX_PORTS][4];
  uint32_t _port_hash_set = 0;

  MeshObserver _obs;
  volatile uint32_t _port_rx[MAX_PORTS] = {0};
  volatile uint32_t _port_tx[MAX_PORTS] = {0};
  volatile uint32_t _port_last_ms[MAX_PORTS] = {0};
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
