#pragma once

// SharedRadio lets several mesh::Mesh instances share one physical LoRa radio
// on one board. Each instance is a separate identity.
//
// The real radio driver owns one RX/TX state machine. Therefore two
// Dispatchers cannot both drive it. Instead, the SharedRadioCore owns the real
// driver. Each mesh gets its own RadioPort, which is a mesh::Radio, and every
// port passes its work through the core:
//
//   * RX fan-out. The core pumps the real radio one time in each super-loop.
//     It buffers each received frame with a snapshot of the SNR and the RSSI.
//     It then delivers that same frame to every port exactly one time. Both
//     identities therefore see all the traffic. Each identity then decides on
//     its own whether the frame is addressed to it, and whether to forward it.
//
//   * TX serialization. Only one port may transmit at a time. The first port
//     that starts a send becomes the owner. While that port holds the
//     transmitter, startSendRaw() returns false for the other ports, and their
//     Dispatcher discards the frame and retries. The isReceiving() of those
//     ports also returns true, so the existing CAD and back-off code defers
//     them correctly. Directly before a send, the core applies the TX power of
//     that port. That power is the per-persona offset plus the jitter.
//
//   * DUTY-CYCLE BUDGET. The core pools this budget, because the duty cycle is
//     a property of the radio and not of an identity. The Dispatcher keeps a
//     tx_budget_ms for each Mesh instance. Therefore N identities behind one
//     antenna give N budgets for one transmitter. Three slots that each think
//     they hold 50% duty would put the node on air at 150%. See
//     setDutyCycle().
//
//   * PRIORITY. The code can rank the ports. Routed repeater traffic is then
//     not held behind a chat identity that transmits frequently. Upstream has
//     priority WITHIN a Dispatcher, through ACTION_RETRANSMIT_DELAYED(0, d).
//     This is the cross-identity equivalent, which upstream does not have.
//
// Everything here is cooperative and single-threaded, and the main loop drives
// it. No interrupt changes the shared state, so the code needs no lock.
//
// The super-loop order MUST be: meshA.loop(); meshB.loop(); shared_core.pump();
// Both ports then take the current frame before the core fetches the next one.

#include <Arduino.h>   // millis(): another header supplies it on ESP32, but not on nRF52
#include "MeshObserver.h"
#include <Mesh.h>
#include <MeshCore.h>

class SharedRadioCore;

class RadioPort : public mesh::Radio {
public:
  RadioPort() : _core(nullptr), _last_snr(0), _last_rssi(0), _last_cr(0), _tx_power_dbm(0) {}
  void attach(SharedRadioCore* core) { _core = core; }

  // The per-persona TX power. The core applies it directly before this port
  // transmits.
  void setPortTxPower(int8_t dbm) { _tx_power_dbm = dbm; }
  int8_t portTxPower() const { return _tx_power_dbm; }

  // the mesh::Radio interface
  void begin() override;   // calls the begin() of the real driver one time. That attaches the DIO1 ISR.
  int recvRaw(uint8_t* bytes, int sz) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override { return true; }   // in logic, the port always listens
  bool isReceiving() override;

  uint32_t getEstAirtimeFor(int len_bytes) override;
  uint32_t getEstAirtimeForCR(int len_bytes, uint8_t cr) override;
  float packetScore(float snr, int packet_len) override;
  int getNoiseFloor() const override;
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override;
  void resetAGC() override;
  void loop() override {}   // SharedRadioCore::pump() pumps the real radio in one central place

  float getLastRSSI() const override { return _last_rssi; }
  float getLastSNR() const override { return _last_snr; }
  // The CR register of the modem holds the value for the frame that the modem
  // decoded last. Behind a queue, that frame is rarely the frame that this
  // identity holds. Therefore the code carries the value with the frame. It
  // does not read the register when the identity asks.
  uint8_t getLastRxCodingRate() const override { return _last_cr; }

  // The core calls this when it delivers a buffered frame to this port.
  void setLastMetadata(float snr, float rssi, uint8_t cr, bool loopback) {
    _last_snr = snr; _last_rssi = rssi; _last_cr = cr; _last_loopback = loopback;
  }

  // Did the frame that recvRaw() gave out last come from a sibling identity on
  // this board, and not from the air? The value stays correct until this port
  // takes the next frame. A Dispatcher reads the other link metrics in the same
  // window, directly after recvRaw(). See LoopbackForwardGuard.
  bool lastRxWasLoopback() const { return _last_loopback; }

  // What the prefs of this identity asked for. The core decides what the
  // shared radio does. See applyRadioPolicy.
  bool wantsCAD() const { return _cad_want; }
  int  wantedThreshold() const { return _thresh_want; }

private:
  SharedRadioCore* _core;
  float _last_snr, _last_rssi;
  uint8_t _last_cr;
  int8_t _tx_power_dbm;
  bool _last_loopback = false;
  bool _cad_want = false;
  int  _thresh_want = 0;
};

/* Optional. Told when the REAL radio transmits or receives a frame -- once per
   frame on the air, not once per port, so a multi-identity node does not report
   a single reception once for every identity it runs. Deliberately tiny and
   free of any LED or Arduino type, so SharedRadio goes on knowing nothing
   about what is listening to it. */
class RadioActivitySink {
public:
  virtual ~RadioActivitySink() { }
  virtual void onRadioTx() { }
  virtual void onRadioRx() { }
};

// This interface is optional. It sets the TX power of the real radio.
// RadioLibWrapper implements setTxPower. The interface stays very small, so
// that SharedRadio does not depend on the concrete wrapper type.
class TxPowerControl {
public:
  virtual void applyTxPower(int8_t dbm) = 0;
};

// ---- packet trace size ----------------------------------------------------
// PKT_TRACE_ENTRIES is the depth of the ring. A value of 0, or no definition
// at all, removes the buffer, the writer and the reader from the build
// completely. It does not leave an array of zero length that still has a cost.
// The default is off. At the full 48 x 200 the trace uses 10.9 KB of RAM, and
// the node carries that RAM whether or not a person is debugging. That is most
// of the apparent cost of a shared radio.
//
// This flag is NOT gated on MESH_DEBUG. MESH_DEBUG is the serial logging
// switch of the project. If the 10.9 KB buffer depended on it, you could not
// have the trace without a large volume of serial output. You also could not
// have serial output without the buffer.
//
// The raw capture has its own flag, because the two controls answer different
// questions, and the cost is the product of the two. The question "did packets
// flow, and when" needs depth, and it needs only the header. The question
// "what exactly was in that frame" needs the bytes, and only for a small
// number of frames. One flag would force you to trade history against detail
// at a fixed ratio.
#ifndef PKT_TRACE_ENTRIES
  #define PKT_TRACE_ENTRIES 0
#endif
#ifndef PKT_TRACE_RAW_CAP
  #define PKT_TRACE_RAW_CAP 200
#endif

// One row of the radio packet trace that SharedRadioCore keeps for
// diagnostics.
// dir: -1 = received. A value of 0 or more = the port with that index
//      transmitted it.
// flag: 0 = ok, 1 = an RX decode or CRC failure, 2 = the TX never completed
//      (it timed out).
#define PKT_FLAG_OK      0
#define PKT_FLAG_RX_ERR  1
#define PKT_FLAG_TX_FAIL 2
#define PKT_FLAG_TX_BUSY 3   // the code deferred the send: a sibling identity held the radio
// This is the RADIOLIB_ERR_CRC_MISMATCH of RadioLib, in the form that it takes
// in aux for an RX_ERR entry. It is the one receive failure where the LoRa
// header still decoded. You can trust the length and the coding rate, because
// only the payload was damaged. In every other failure the header itself was
// lost, so we must not report any value that the code read from it.
#define PKT_RX_ERR_CRC   (-7)

// The reason why a transmission did not happen. Without these counters, a node
// that never gets access to the channel looks the same as a node on a quiet
// band. Both of them transmit nothing. TXWAIT_RX_PACKET, TXWAIT_RSSI and
// TXWAIT_CAD are three separate conditions behind the single isReceiving()
// bool of the driver. They have very different meanings. They are a neighbour
// in the middle of a packet, an interference threshold that is set too low,
// and the hardware CAD.
#define TXWAIT_BUDGET      0   // the pooled duty-cycle budget is empty
#define TXWAIT_SIBLING     1   // another identity on this board held the transmitter
#define TXWAIT_PRIORITY    2   // the port yielded to an identity of higher priority
#define TXWAIT_RX_PACKET   3   // the channel is busy: the radio detected a preamble or a header
#define TXWAIT_RSSI        4   // the channel is busy: the RSSI is above the noise floor plus the threshold
#define TXWAIT_CAD         5   // the channel is busy: the hardware CAD reported it as occupied
#define TXWAIT_RADIO       6   // the driver itself refused the send
#define TXWAIT_FORCED      7   // the port sent the frame over a live claim of higher priority
#define TXWAIT_NUM         8

struct PktLogEntry {
  uint32_t seq;
  uint32_t t_ms;
  int8_t   dir;
  uint8_t  flag;
  uint8_t  hdr;      // the raw packet header byte (the route and type bits)
  uint8_t  len;      // the full over-the-air length
  int8_t   snr4;     // SNR * 4 (RX only)
  int16_t  rssi;     // RX only
  int16_t  aux;      // this depends on the flag: for RX_ERR it is the RadioLib error code
  // The first 4 bytes of the packet hash of MeshCore
  // (Packet::calculatePacketHash). The hash covers the payload and NOT the
  // path. That is the purpose of it. Every copy of one flood carries the same
  // hash, whatever number of hops it has travelled. Therefore you can see at
  // once which frames are repeats of a packet that the mesh forwards, and
  // which frames are new traffic.
  //
  // The code computes the hash here, not in the reader, because raw[] is
  // truncated at PKT_TRACE_RAW_CAP. Downstream, the payload of a long packet
  // is no longer complete. A hash taken there would be wrong, and nothing
  // would report the error.
  uint8_t  hash[4];
  bool     hash_ok;  // false when the code could not parse a payload out of the frame
  // The time on air in ms. It comes from the airtime sum of the radio driver.
  // The code does not implement the formula again. The driver sum already
  // includes the preamble, the CRC, the explicit header and the low-data-rate
  // optimisation. The code prices a transmission at the settings that we sent
  // it with. It prices a receive at the CR from the header of THAT frame (see
  // cr below). Therefore the code costs a neighbour on a different coding rate
  // at what that neighbour truly spent on the channel, and not at what we
  // would have spent. The SF and the BW must match ours, or the packet would
  // not have decoded. The preamble length is the one term that LoRa does not
  // transmit, so the code assumes our own value.
  uint16_t air_ms;
  // The coding rate as the 4/x denominator (5..8). It is 0 when the code does
  // not know it. For a received frame this is the CR that the SENDER used. The
  // code reads it out of the explicit LoRa header that the radio just decoded.
  // It is not our own setting. Therefore a neighbour that runs a different CR
  // appears as such. The code does not assume our value for it. For a
  // transmission this is the CR that the shared radio is configured with.
  uint8_t  cr;
  uint8_t  raw_len;  // the bytes captured in raw[]. It is <= len, and capped at PKT_TRACE_RAW_CAP.
  uint8_t  raw[PKT_TRACE_RAW_CAP];
};

class SharedRadioCore {
public:
  static const int MAX_PORTS = 8;
  // The depth of the ring. It is 0 when the build removes the trace. Every
  // reader below then reports an empty result. No reader fails to build.
  static const int PKT_LOG_SIZE = PKT_TRACE_ENTRIES;
  static const int PKT_LOG_RAW_CAP = PKT_TRACE_RAW_CAP;
  static bool pktTraceEnabled() { return PKT_TRACE_ENTRIES != 0; }
  // The code QUEUES the received frames. It does not hold one frame at a time.
  // It empties the radio at the moment a packet arrives, and the identities
  // then take frames from the queue at their own speed. Before this change,
  // the code could not fetch the next packet until every identity had taken
  // the current one. A dispatcher that waits on its own transmit skips its
  // receive step completely. Therefore one busy identity stopped reception for
  // all of them, and packets were lost in the FIFO of the radio.
  static const int RX_SLOTS = 6;

  explicit SharedRadioCore(mesh::Radio& real) : _real(&real), _num_ports(0),
      _tx_owner(nullptr), _pwr_ctl(nullptr), _applied_pwr(0x7F) {}

  void setTxPowerControl(TxPowerControl* ctl) { _pwr_ctl = ctl; }
  void setActivitySink(RadioActivitySink* sink) { _activity = sink; }

  // You may register a port as inactive, and you may switch a port on and off
  // while the node runs. Therefore you can start or silence an identity
  // without a reboot. The indices stay stable, because the packet trace and
  // the TX-owner records refer to them. The delivery mask excludes an inactive
  // port. Therefore the code never holds a frame while it waits for a port
  // that does not listen.
  int addPort(RadioPort& port, bool active = true) {   // returns the port index
    int idx = _num_ports;
    _ports[_num_ports++] = &port;
    port.attach(this);
    setPortActive(idx, active);
    return idx;
  }
  void setPortActive(int idx, bool active);
  bool portActive(int idx) const;

  // ---- pooled duty-cycle budget --------------------------------------------
  // The factor is the airtime budget factor of the Dispatcher. The duty is
  // 1/(1+factor), so a factor of 1.0 gives 50%. That is also the default here,
  // and it matches Dispatcher::getAirtimeBudgetFactor(). Therefore a
  // composition that forgets to call this function still has a limit.
  //
  // The code charges the pool the ESTIMATED airtime of a frame before that
  // frame reaches the air. It does not charge the measured airtime afterwards.
  // A send that the TX watchdog releases later still used the channel. A budget
  // that debits only on a clean completion would not have counted it.
  void setDutyCycle(float airtime_budget_factor, uint32_t window_ms = 3600000);
  uint32_t txBudgetMs();                 // it refills first, so this is the live value
  uint32_t txBudgetMaxMs() const { return _duty_max_ms; }
  // The airtime that the code has charged to the pool since the boot. It never
  // exceeds what the duty cycle permits over the elapsed window. That is the
  // invariant that the tests check.
  uint32_t txChargedMs() const { return _tx_charged_ms; }

  // ---- cross-identity priority ---------------------------------------------
  // A value of 0 is the highest priority. The default is 1. The priority is
  // per port, and not a rule that slot 0 always wins. The arbiter does not know
  // what a slot is, and a room server needs the same standing as a repeater. A
  // port that wanted the air and did not get it leaves a CLAIM. The ports of
  // lower priority then report isReceiving() until the claim expires, or until
  // the port that made the claim transmits. Their dispatchers therefore back
  // off correctly. The priority never REFUSES a send. The code lets through a
  // dispatcher that has used its full CAD-busy timeout and then force-sent.
  // That limits how long one identity can be held off to the 4 s that the
  // timeout already permits.
  void setPortPriority(int idx, uint8_t pri);
  uint8_t portPriority(int idx) const;

  // ---- deferral accounting --------------------------------------------------
  uint32_t txWaits(int reason) const {
    return (reason >= 0 && reason < TXWAIT_NUM) ? _tx_waits[reason] : 0;
  }
  // This probe divides the isReceiving() of the driver into the three
  // conditions below it. The composition supplies the probe, because the work
  // needs the concrete radio wrapper, and this file knows nothing about that
  // wrapper. The probe returns TXWAIT_RX_PACKET, TXWAIT_RSSI or TXWAIT_CAD.
  typedef int (*ChannelBusyProbe)();
  void setChannelBusyProbe(ChannelBusyProbe fn) { _busy_probe = fn; }

  // Pump the real radio one time. If the code has delivered the current frame
  // to every port, it fetches the next frame. Call this AFTER every mesh has
  // had its loop() in this cycle.
  void pump();

  // --- RadioPort calls these ---
  void beginRealOnce();
  int  takeFrame(RadioPort* p, uint8_t* dst, int sz);
  bool tryStartSend(RadioPort* p, const uint8_t* bytes, int len);
  bool isSendCompleteFor(RadioPort* p);
  void onSendFinishedFor(RadioPort* p);
  bool txBusyForOthers(RadioPort* p) const { return _tx_owner != nullptr && _tx_owner != p; }
  bool txInFlight() const { return _tx_owner != nullptr; }
  // This holds all of RadioPort::isReceiving(). One place therefore decides
  // and counts every reason why the code tells a port to wait.
  bool portMustWait(RadioPort* p);

  mesh::Radio* real() { return _real; }

  // --- The packet trace. It is a ring with one writer. A reader can call
  //     pktLogCopy() safely. ---
#if PKT_TRACE_ENTRIES
  uint32_t pktLogSeq() const { return _pkt_seq; }
#else
  uint32_t pktLogSeq() const { return 0; }
#endif
  uint32_t rxTotal() const { return _rx_total; }
  uint32_t txTotal() const { return _tx_total; }
  // The sends that the code refused because a sibling identity held the
  // transmitter. This is the cost of a shared radio. Until now nothing showed
  // it, because the dispatcher backs off and retries and reports nothing.
  uint32_t txContention() const { return _tx_contention; }
  // The transmits that the watchdog below released. See pump().
  uint32_t txStuck() const { return _tx_stuck; }
  // The sends that the radio itself refused with a RadioLib error. These are
  // true TX failures.
  uint32_t txRefused() const { return _tx_refused; }
  // The number of times the code initialised the radio again after the radio
  // went silent.
  uint32_t radioRecoveries() const { return _radio_recoveries; }
  // The time on air, transmit plus receive, as the REAL radio saw it. The
  // design makes this value node-wide, because the traffic of every identity
  // passes through here. The LoRa watchdog of the node uses this value as its
  // input (helpers/LoraWatchdog.h). A Dispatcher can account only for the
  // transmit share of its own identity. The code prices a receive at the CR
  // from the header of the sender. It prices a transmit at the estimate that
  // it charged to the duty-cycle pool.
  uint32_t airtimeMs() const { return _tx_air_ms + _rx_air_ms; }
  uint32_t txAirtimeMs() const { return _tx_air_ms; }
  uint32_t rxAirtimeMs() const { return _rx_air_ms; }
  uint32_t msSinceLastRx() const { return _last_rx_ms ? (uint32_t)(millis() - _last_rx_ms) : 0; }

  // COLLISION AVOIDANCE IS A PROPERTY OF THE RADIO, NOT OF AN IDENTITY.
  // The dispatcher of each stock mesh writes its own cad_enabled and
  // interference_threshold prefs to its radio every few seconds. With one
  // radio behind three dispatchers, three writers change one setting, and the
  // writer that ran last decides the value. Therefore an identity with CAD off
  // disables CAD for the others, and it reports nothing. The RSSI threshold
  // also moves between three values. A transmission on top of a packet that is
  // already in the air damages both packets. That is the exact failure that we
  // want to remove. Therefore the core uses the most careful setting that any
  // listening identity asked for.
  void applyRadioPolicy(bool recalibrate);
  bool cadEnabled() const { return _cad_applied > 0; }
  int  interferenceThreshold() const { return _thresh_applied; }
  // The frames that the code discarded because every identity emptied the
  // queue too slowly.
  uint32_t rxDropped() const { return _rx_dropped; }
  int rxQueued() const { return _rx_count; }
  // How the composition assembles a wedged transceiver again.
  void setRadioReinit(void (*fn)()) { _reinit_fn = fn; }

  // ---- raw frame hook -------------------------------------------------------
  // This hook receives every frame that the arbiter handles, exactly one time.
  // It serves any code that wants to watch the traffic without being an
  // identity. The arbiter works in raw frames, because it sits below the
  // Packet layer. Therefore the hook passes bytes, and the caller decides
  // whether to parse them. It is a plain function pointer, so this file stays
  // free of whatever the composition attaches, for example a bridge or an
  // uplink.
  typedef void (*FrameHook)(const uint8_t* frame, int len, bool is_tx, float snr, float rssi);
  void setFrameHook(FrameHook fn) { _frame_hook = fn; }

  // ---- relay confirmation --------------------------------------------------
  // Did any node truly hear us? When another node relays a flood that we sent,
  // that node adds its own hash and transmits the frame again. Therefore a
  // received packet that carries OUR hash in its path is proof that a
  // neighbour received our transmission and passed it on. Give the arbiter the
  // public key of each port, so that it can find those packets.
  //
  // The code counts the matches separately by hash width, because the widths
  // are not equally reliable. A 1-byte hash collides approximately 1 time in
  // 256 for each hop. With several hops in each packet, a large number of
  // those "confirmations" are coincidences. A 2-byte match collides
  // approximately 1 time in 65536, which is near to certain.
  void setPortIdentity(int idx, const uint8_t* pub_key) {
    if (idx < 0 || idx >= MAX_PORTS || pub_key == nullptr) return;
    memcpy(_port_hash[idx], pub_key, 4);
    _port_hash_set |= (1u << idx);
    _obs.addSelfKey(pub_key);   // it can then separate "a node relayed US" from noise
  }
  uint32_t floodsSent(int idx) const { return _obs.floodsSent(idx); }
  uint32_t floodsConfirmed(int idx) const { return _obs.floodsConfirmed(idx); }
  uint32_t confirmsByWidth(int bytes) const { return _obs.confirmsByWidth(bytes); }

  // ---- observability --------------------------------------------------------
  // The peer table and the hop and packet-type histograms are in MeshObserver.
  // That class knows nothing about arbitration. A single-identity node uses
  // the same code with a plain receive hook. The arbiter only gives it every
  // frame that the arbiter pumps.
  MeshObserver& observer() { return _obs; }
  const MeshObserver& observer() const { return _obs; }
  typedef MeshObserver::PeerEntry PeerEntry;
  int numPeers() const { return _obs.numPeers(); }
  const PeerEntry* peer(int i) const { return _obs.peer(i); }
  int confirmedPeerCount() const { return _obs.confirmedPeerCount(); }

  // ---- per-identity liveness -----------------------------------------------
  // Every identity passes through a port. Therefore the arbiter can report
  // what each identity does. This includes the chat identities, which have no
  // CLI and therefore no stock statistics of their own.
  int numPorts() const { return _num_ports; }
  uint32_t portRxCount(int idx) const { return (idx >= 0 && idx < MAX_PORTS) ? _port_rx[idx] : 0; }
  uint32_t portTxCount(int idx) const { return (idx >= 0 && idx < MAX_PORTS) ? _port_tx[idx] : 0; }
  // The ms since this identity last sent or received a frame. 0 means never.
  uint32_t portIdleMs(int idx) const {
    if (idx < 0 || idx >= MAX_PORTS || _port_last_ms[idx] == 0) return 0;
    return (uint32_t)(millis() - _port_last_ms[idx]);
  }
  bool portEverActive(int idx) const { return idx >= 0 && idx < MAX_PORTS && _port_last_ms[idx] != 0; }
  uint32_t txContentionFor(int idx) const {
    return (idx >= 0 && idx < MAX_PORTS) ? _tx_contention_port[idx] : 0;
  }
  // Copy the entries with a seq greater than after_seq into out, oldest first.
  // Returns the number of entries.
  int pktLogCopy(PktLogEntry* out, int max_entries, uint32_t after_seq);
  const char* portName(int idx) const { return (idx >= 0 && idx < _num_ports) ? _port_names[idx] : "?"; }
  void setPortName(int idx, const char* name) { if (idx >= 0 && idx < MAX_PORTS) _port_names[idx] = name; }
  // These accessors let pump() find the RX decode and CRC failures inside the
  // real driver, which reports them only through a counter. pump() then writes
  // them to the trace as events. The code accessor supplies the RadioLib error
  // code, which separates a CRC mismatch from header damage.
  void setRxErrorCounter(uint32_t (*fn)(), int16_t (*code_fn)() = nullptr,
                         const uint8_t* (*payload_fn)() = nullptr, uint8_t (*len_fn)() = nullptr) {
    _rx_err_fn = fn; _rx_err_code_fn = code_fn;
    _rx_err_payload_fn = payload_fn; _rx_err_len_fn = len_fn;
    _rx_err_seen = fn ? fn() : 0;
  }

  // The coding-rate records for the packet trace. The code that configures the
  // radio owns the CR that is in force. That is the shared "set radio" path.
  // Therefore the caller passes the CR in, and this class does not guess it.
  // The RX accessor reads the CR out of the LoRa header of the frame that the
  // radio just decoded. That CR belongs to the sender, not to us. Both values
  // are 4/x denominators (5..8), and 0 means unknown.
  void setCodingRate(uint8_t cr) { _cfg_cr = (cr >= 5 && cr <= 8) ? cr : 0; }
  uint8_t codingRate() const { return _cfg_cr; }

  // TX loopback. The identities on this board share one antenna, and the radio
  // is half-duplex. Therefore no identity ever hears what another identity on
  // the same board transmits. The companion cannot see its own room, the
  // repeater cannot relay for the identities, and so on. With the loopback on,
  // the code also delivers every transmitted frame to the sibling ports as a
  // received frame. It does not deliver the frame to the sender. This gives
  // the identities the experience of a second physical node in the same room.
  void setLoopback(bool on) { _loopback = on; }
  bool loopback() const { return _loopback; }

private:
  int portIndex(RadioPort* p) const {
    for (int i = 0; i < _num_ports; i++) if (_ports[i] == p) return i;
    return -1;
  }
  uint32_t allPortsMask() const { return _active_mask; }   // only a listening port must take a frame

  mesh::Radio* _real;
  RadioPort* _ports[MAX_PORTS];
  int _num_ports;

  struct RxFrame {
    uint8_t  buf[MAX_TRANS_UNIT];
    uint8_t  len;
    uint8_t  cr;             // the 4/x denominator from the header of this frame; 0 = unknown
    bool     loopback;       // a sibling identity on this board sent it, not a remote node
    float    snr, rssi;
    uint32_t consumed;       // the code sets bit i after port i takes this frame
  };
  RxFrame  _rx[RX_SLOTS];
  uint8_t  _rx_head = 0, _rx_count = 0;
  volatile uint32_t _rx_dropped = 0;   // the queue was full: the code discarded the oldest frame
  bool enqueueRx(const uint8_t* bytes, int len, float snr, float rssi, uint8_t cr,
                 uint32_t consumed_init, bool loopback);
  void retireConsumedFrames();

  int8_t   _cad_applied = -1;      // -1 = the code has never applied it
  int      _thresh_applied = 0;
  uint32_t _last_calib_ms = 0;
  static const uint32_t CALIB_MIN_INTERVAL_MS = 2000;   // the stock rate for each mesh

  // ---- pooled duty-cycle budget ----
  void     refillBudget();
  void     noteWait(int reason) { if (reason >= 0 && reason < TXWAIT_NUM) _tx_waits[reason]++; }
  float    _duty = 0.5f;                    // the fraction of the window that we may transmit in
  uint32_t _duty_window_ms = 3600000;
  uint32_t _duty_max_ms = 1800000;
  uint32_t _budget_ms = 1800000;
  uint32_t _budget_last_ms = 0;
  uint32_t _tx_charged_ms = 0;
  // Below this value the pool stops accepting new sends. This value copies the
  // MIN_TX_BUDGET_RESERVE_MS of the Dispatcher, so that the two agree on the
  // meaning of "almost empty". The per-frame test in tryStartSend() is the
  // hard limit. This value is only the low mark that makes the ports back off
  // before they reach that limit.
  static const uint32_t BUDGET_RESERVE_MS = 100;
  volatile uint32_t _tx_waits[TXWAIT_NUM] = {0};
  ChannelBusyProbe _busy_probe = nullptr;

  // ---- cross-identity priority ----
  bool     higherPriorityClaimed(int idx) const;
  uint8_t  _port_pri[MAX_PORTS] = { 1, 1, 1, 1, 1, 1, 1, 1 };
  uint32_t _claim_ms[MAX_PORTS] = {0};      // the last time this port wanted the air and did not get it
  // This time is long enough for the next attempt of a denied port to arrive.
  // The dispatcher of that port retries every getCADFailRetryDelay(), which is
  // 200 ms. It is also short enough that a port that makes a claim and then
  // goes quiet cannot hold the other ports off for a noticeable time.
  static const uint32_t CLAIM_TTL_MS = 1000;

  RadioPort* _tx_owner;
  TxPowerControl* _pwr_ctl;
  RadioActivitySink* _activity = nullptr;
  int8_t   _applied_pwr;     // the last power applied to the real radio. It prevents unnecessary writes.
  bool     _real_begun = false;   // the begin() of the real driver must run exactly one time

  void pktLogAdd(int8_t dir, const uint8_t* bytes, int len, int8_t snr4, int16_t rssi, uint8_t flag = 0, int16_t aux = 0);
  uint32_t (*_rx_err_fn)() = nullptr;
  int16_t (*_rx_err_code_fn)() = nullptr;
  const uint8_t* (*_rx_err_payload_fn)() = nullptr;
  uint8_t (*_rx_err_len_fn)() = nullptr;
  uint32_t _rx_err_seen = 0;
  uint8_t _cfg_cr = 0;                // the CR that the shared radio transmits with

  // The loopback queue. It holds the frames that one port sent, and that wait
  // for delivery to the other ports.
  static const int LB_SLOTS = 4;
  struct LbFrame { uint8_t buf[MAX_TRANS_UNIT]; uint8_t len; int8_t from; };
  LbFrame _lb[LB_SLOTS];
  uint8_t _lb_head = 0, _lb_count = 0;
  bool _loopback = true;
  uint32_t _active_mask = 0;   // the ports that listen now
  bool _tx_completed = false;   // did the send of the present owner reach TX-done?
#if PKT_TRACE_ENTRIES
  PktLogEntry _pkt_log[PKT_TRACE_ENTRIES];
  volatile uint32_t _pkt_seq = 0;   // the total packets ever logged. The ring index is seq % SIZE.
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

  // the records for the relay confirmation
  uint8_t  _port_hash[MAX_PORTS][4];
  uint32_t _port_hash_set = 0;

  MeshObserver _obs;
  volatile uint32_t _port_rx[MAX_PORTS] = {0};
  volatile uint32_t _port_tx[MAX_PORTS] = {0};
  volatile uint32_t _port_last_ms[MAX_PORTS] = {0};
  // How long the code permits a silent radio before it decides that the radio
  // is wedged. This time is long enough that a truly quiet band never trips
  // it. It is also short enough that the node is not off the air for hours.
  static const uint32_t RX_SILENCE_LIMIT_MS = 900000;   // 15 minutes
  // The longest time that a port may hold the transmitter before the arbiter
  // takes it back. It is well above any legal LoRa airtime. A 255-byte frame
  // at SF12/BW125 needs approximately 9s. This value breaks a deadlock. It is
  // not a timing parameter.
  static const uint32_t TX_HOLD_LIMIT_MS = 15000;
  volatile uint32_t _tx_contention_port[MAX_PORTS] = {0};
  const char* _port_names[MAX_PORTS] = { "?", "?", "?", "?", "?", "?", "?", "?" };
};

// ---- loopback frames must not be relayed -----------------------------------
//
// The loopback above gives an identity the EXPERIENCE of a second node in the
// room, but it is not one. A node that can hear the room slot of this board can
// also hear the repeater of this board: same antenna, same radio horizon.
// Therefore a relay of a sibling transmission adds no coverage. It only spends
// airtime from the pooled duty budget and puts an extra hash in the path, which
// makes one board look like two hops.
//
// So the delivery stays and the FORWARD stops. This class carries "the frame
// came from a sibling" from the moment the port hands the frame over to the
// moment the mesh decides whether to retransmit it. The two moments are not the
// same moment: a flood packet can wait in the delayed inbound queue of the
// Dispatcher for as long as calcRxDelay() says. A flag on the port alone would
// be stale by then.
//
// The identity of a packet here is its POINTER. The packet pool owns the
// memory, so a pointer is stable while the packet lives, and a pointer that
// comes back from the pool always passes through onRx() again before anything
// asks about it. That call rewrites the entry. Therefore a recycled address
// cannot inherit a mark from the packet that held it before.
//
// A Mesh subclass wires up three of its own virtuals:
//   logRx()              -> onRx(pkt, port.lastRxWasLoopback())
//   onRecvPacket()       -> beginProcess(pkt) ... endProcess()
//   allowPacketForward() -> return !blocksForward() && Base::allowPacketForward()
namespace mesh { class Packet; }

class LoopbackForwardGuard {
public:
  // The marks that this class can hold at one time. One mark is live only
  // between the receive of a packet and the decision about it. That window
  // holds the delayed inbound queue and nothing else.
  static const int MARK_SLOTS = 8;

  // Call this for EVERY received packet, and not only for the loopback ones.
  // A call with loopback=false is what clears a stale mark off a pointer that
  // the pool has given out again.
  void onRx(const mesh::Packet* pkt, bool loopback) {
    if (pkt == nullptr) return;
    int at = find(pkt);
    if (!loopback) { if (at >= 0) drop(at); return; }
    if (at >= 0) return;                       // already marked
    if (_num == MARK_SLOTS) { drop(0); _overflows++; }   // the oldest mark gives way
    _mark[_num++] = pkt;
  }

  // Take the mark of the packet that the mesh is about to decide on.
  void beginProcess(const mesh::Packet* pkt) {
    int at = find(pkt);
    _blocking = (at >= 0);
    if (at >= 0) drop(at);
  }
  void endProcess() { _blocking = false; }
  bool blocksForward() const { return _blocking; }

  // The marks that MARK_SLOTS could not hold. Each one is a sibling frame that
  // this node may have relayed after all. It should stay at zero.
  uint32_t overflows() const { return _overflows; }
  int numMarks() const { return _num; }

private:
  int find(const mesh::Packet* pkt) const {
    for (int i = 0; i < _num; i++) if (_mark[i] == pkt) return i;
    return -1;
  }
  void drop(int at) {
    for (int i = at + 1; i < _num; i++) _mark[i - 1] = _mark[i];
    _num--;
  }

  const mesh::Packet* _mark[MARK_SLOTS] = {nullptr};
  int _num = 0;
  bool _blocking = false;
  uint32_t _overflows = 0;
};
