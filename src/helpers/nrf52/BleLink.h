#pragma once

#include <stdint.h>
#include <bluefruit.h>

/**
 * @brief  Connection-oriented peer links, which carry the bridge frames.
 *
 * Why a connection and not a broadcast: an earlier version of this transport
 * sent each frame as a BLE 5 extended advert, and that cannot be made reliable.
 * Measured on a live pair, a single advert lands about half of the time. The
 * only defence is to send each frame three to five times, which moves the loss
 * from 49% to 3.4% but never to zero, because nothing acknowledges anything.
 * The alternatives took a while to exclude: CPU (2us for each report), the BLE
 * central connection (no effect), our own transmissions (the node was idle) and
 * foreign advert traffic (86% removed, no improvement) were each excluded by
 * measurement. What remains is intrinsic to extended advertising.
 *
 * A connection is different in kind. The link layer acknowledges every packet
 * and retransmits it until the peer receives it or the supervision timeout
 * ends. Delivery is reliable and ordered, and the retries cost us nothing.
 *
 * Two more gains follow. MAX_FRAME is 256, so a full MeshCore packet fits; the
 * advert path carried 236 bytes and discarded a packet with a long path in
 * silence. And fragmentation over a connection is routine, because the peer
 * acknowledges each fragment; over a broadcast it was fatal.
 *
 * The cost is that a connection is point to point. This class thus cannot find
 * a peer by itself. BleDiscovery finds one and the bridge calls notePeer().
 */
class BleLink {
public:
  /** Delivered frames, with the link that carried each one, so a caller does
   *  not echo a frame straight back to its source. */
  typedef void (*rx_handler_t)(const uint8_t* data, uint16_t len, uint8_t link_idx);

  /** Asked before we adopt a peer that dialled IN. The bridge owns the deny
   *  list, so BleLink asks rather than keeping a list of its own.
   *  @returns false to refuse the peer, which is then disconnected. */
  typedef bool (*allow_handler_t)(const ble_gap_addr_t& addr);

  /* Outward links. Bounded by the central slots that the SoftDevice granted,
     which on a RAK3401 came to three. See BleStack. */
  static const uint8_t MAX_LINKS = 3;
  /* Index that means "no link": a locally sourced frame excludes nothing. */
  static const uint8_t NO_LINK = 0xFF;
  /* Index of the inbound peer, which has no Link slot of its own.
     Deliberately distinct from NO_LINK. An earlier version used NO_LINK for
     both meanings and had to write `except != NO_LINK - 1` to keep keepalive
     traffic flowing to the inbound peer. One value for one thing. */
  static const uint8_t INBOUND_LINK = 0xFE;

  /* A frame is a bridge datagram: header, packet and tag. */
  static const uint16_t MAX_FRAME = 256;

  /**
   * @param handler    called from the BLE event context. Keep it short.
   * @param self_addr  our own BLE address, for the tie-break below.
   * @param allow      optional: asked before a peer that dialled in is adopted.
   */
  bool begin(rx_handler_t handler, const ble_gap_addr_t& self_addr,
             allow_handler_t allow = nullptr);

  /** Drive connection attempts, the idle check and the transmit queues. Call
   *  this from the main loop. */
  void loop();

  /** Tear every link down and stop. This disconnects and does not merely
   *  forget: a link that stays connected holds a peripheral slot on the far
   *  side, and a node with no free peripheral slot stops to advertise. */
  void end();

  /** Send to every link, whatever the source. Used for keepalive frames. */
  uint8_t sendKeepalive(const uint8_t* data, uint16_t len);

  /**
   * @brief  A peer that we may dial.
   *
   * BleDiscovery supplies this from a beacon that carries our group marker.
   * The marker is public, so it filters and does not authenticate. The bridge
   * proves group membership on the first frame over the link, and it drops and
   * deny-lists a link that fails. See BLEBridge.
   */
  void notePeer(const ble_gap_addr_t& addr);

  /**
   * @brief  Send to every established link except one.
   *
   * @param except  the link that the frame arrived on, INBOUND_LINK for the
   *                inbound peer, or NO_LINK for a locally sourced frame. This
   *                is not a loop guard on its own -- the packet hash tables are
   *                that -- but it stops the pointless immediate echo.
   * @returns how many links accepted the frame.
   */
  uint8_t send(const uint8_t* data, uint16_t len, uint8_t except = NO_LINK);

  /** True one time after a link came up or went down. The caller must arm the
   *  scanner again, because a connect attempt stopped it in silence. */
  bool takeTopologyChanged() {
    bool c = _topology_changed; _topology_changed = false; return c;
  }

  /** Called by the bridge when a frame from this link passes the group tag.
   *  That tag is the only proof that the peer belongs to our bridge.
   *  INBOUND_LINK refers to the inbound peer. */
  void markAuthed(uint8_t idx);

  /**
   * @brief  Disconnect one link and hold it down for a long backoff.
   *
   * The bridge calls this when the first frame on a link fails the group tag.
   * A peer that cannot authenticate now will not authenticate in two seconds
   * either, so the link goes to the maximum backoff.
   */
  void dropLink(uint8_t idx);

  /**
   * @brief  Take the address of a link that we dropped for no authentication.
   *
   * The bridge owns the deny list, so BleLink reports each drop instead of
   * keeping a list of its own.
   *
   * @returns false when nothing is waiting.
   */
  bool takeAuthFailure(ble_gap_addr_t& addr);

  uint8_t numUp() const;
  /** The address of the inbound peer, if one is attached.
   *  @param rx_age_s  optional: seconds since the last frame arrived, or
   *                    0xFFFFFFFF if nothing ever has.
   *  @param queued     optional: frames still waiting in its TX queue. */
  bool getInboundAddr(ble_gap_addr_t& addr, uint32_t* rx_age_s = nullptr,
                      uint32_t* queued = nullptr) const;
  /** @param rx_age_s  optional: seconds since the last frame arrived, or
   *                    0xFFFFFFFF if nothing ever has.
   *  @param queued     optional: frames still waiting in this link's TX queue. */
  bool getLink(uint8_t idx, ble_gap_addr_t& addr, bool& up, int8_t& rssi,
               uint32_t& sent, uint32_t& recv, uint32_t& drops,
               uint32_t* rx_age_s = nullptr, uint32_t* queued = nullptr) const;

private:
  /* Only the node with the numerically lower address dials. A pair thus agrees
     on exactly one link, instead of two crossed attempts that each tear the
     other down. No negotiation and no timers: both ends reach the same answer
     from information that they already hold. */
  bool weInitiateTo(const ble_gap_addr_t& peer) const;

  enum State : uint8_t { EMPTY = 0, IDLE, CONNECTING, DISCOVERING, UP };

  /* Framing: [SYNC][len lo][len hi].
     The sync byte exists because the first assumption -- "the link is reliable
     and ordered, so a length prefix is all the framing that we need, with no
     gap handling and no timeouts" -- fails in the one case that matters.
     The sender used to abandon a frame when the stack refused a chunk, with the
     header ALREADY on the air, and the receiver had no way to know. With no
     marker the next frame goes into the abandoned one, and the stream loses
     synchronisation permanently: every later frame reassembles across a
     boundary, still looks well formed, and fails its HMAC. The split bad-tag
     counter caught exactly that: 0 on the broadcast path, and a steady climb on
     the link. With a marker the receiver hunts for the next SYNC and recovers
     on the very next frame. */
  static const uint8_t  FRAME_SYNC = 0xA5;
  static const uint8_t  HDR_SIZE = 3;
  /* Frames buffered for each destination. Four is about a second of bridge
     traffic at the rates that we observed, which covers the 20ms to 30ms that a
     connection event takes to give TX credits back. */
  static const uint8_t  TXQ_DEPTH = 4;

  struct Link {
    ble_gap_addr_t addr;
    uint16_t conn;
    State state;
    unsigned long next_try_ms;
    uint16_t backoff_ms;
    uint32_t sent, recv, drops;
    /* A connection can stay nominally up and carry nothing. The supervision
       timeout notices a radio that went away, not a peer that stopped to talk.
       Heartbeats give the link a floor of one frame for each interval, so
       silence beyond a minute means the link is dead and worth a redial rather
       than a slot held open. */
    unsigned long last_rx_ms;
    /* Reassembly. The stack caps a write at the ATT payload, which is 20 bytes
       on the default 23-byte MTU, so a frame arrives in pieces. */
    uint16_t rx_expect;
    uint16_t rx_have;
    uint8_t rx_buf[MAX_FRAME];
    /* Header bytes collected so far. A header CAN straddle two writes. The old
       code simply returned when fewer than two bytes remained, discarded them,
       and started the next write in the middle of a header. */
    uint8_t rx_hdr[3];
    uint8_t rx_hdr_have;
    /* Group membership, proven by one frame that passed the bridge's tag check.
       BLEBridge owns the key and sets this through markAuthed(). */
    bool authed;
    unsigned long up_ms;
    uint32_t unauthed_drops;
    /* When the frame in progress started, for the staleness timeout. */
    unsigned long rx_started_ms;
    uint32_t rx_resyncs;

    /* Outbound queue. send() copies a fully framed datagram in and returns at
       once; loop() pushes it out as TX credits allow and resumes from tx_off
       wherever the last pass stopped.

       Nothing here blocks, and that is the point. The BLE event task can reach
       send() at TASK_PRIO_HIGH, so the earlier retry with delay() stalled a
       context that preempts both the main loop and the watchdog that must
       notice a stall. */
    uint8_t  txq[TXQ_DEPTH][MAX_FRAME + HDR_SIZE];
    uint16_t txq_len[TXQ_DEPTH];
    uint16_t tx_off;                     // progress into the head entry
    uint8_t  txq_head, txq_count;
    uint32_t tx_dropped;                 // queue full: honest backpressure
  };

  /* A frame that stops to arrive is abandoned, and we do not wait for bytes
     that never come. Generous: it only has to exceed the gap between frames,
     and it meets no transmission deadline. */
  static const uint32_t RX_STALE_MS = 3000;

  /* How long a link may stay up with no frame that passes the group tag.
     Comfortably longer than the 15s heartbeat, so an ordinary peer is never at
     risk, and short enough that a foreign peer does not squat on a slot. */
  static const uint32_t AUTH_GRACE_MS = 45000;

  static const uint32_t LINK_IDLE_LIMIT_MS = 60000;

  /* How long a connection handle stays refused after we dropped it. A
     disconnect is asynchronous, so a write from the peer we just dropped can
     still arrive and would otherwise buy it a new authentication window. Two
     seconds covers the disconnect and stays well below the interval at which
     the SoftDevice would give the same handle to somebody else. */
  static const uint32_t IN_DROP_HOLD_MS = 2000;

  static const uint16_t BACKOFF_MIN_MS = 2000;
  static const uint16_t BACKOFF_MAX_MS = 60000;

  /* Addresses dropped for no authentication, waiting for the bridge to read
     them. Four is one for every link plus the inbound peer, so a pass that
     drops everything loses nothing. */
  static const uint8_t AUTH_FAIL_DEPTH = 4;

  void onConnected(uint16_t conn);
  void onDisconnected(uint16_t conn, uint8_t reason);
  void onNotify(uint8_t idx, const uint8_t* data, uint16_t len);
  void onWritten(uint16_t conn, const uint8_t* data, uint16_t len);
  void feed(Link& l, uint8_t idx, const uint8_t* data, uint16_t len);
  /* Shared by the outward links and the inbound peer. See the .cpp. */
  void reassemble(uint8_t* hdr, uint8_t& hdr_have,
                  uint16_t& expect, uint16_t& have, uint8_t* buf,
                  unsigned long& started_ms, uint32_t& resyncs,
                  uint32_t& recv, unsigned long& last_rx_ms,
                  uint8_t idx, const uint8_t* data, uint16_t len);
  /**
   * @brief  Return one outward link to IDLE with every buffer empty.
   *
   * Every teardown path calls this, so a new path cannot forget a field. Two
   * of the three paths used to leave the TX queue behind. A part-sent frame
   * then resumed from tx_off on the next connection and put a headerless tail
   * on the wire; a tail that carries a plausible SYNC and length builds a
   * frame that fails the group tag, and that count is what separates a foreign
   * secret from a framing fault.
   *
   * The caller keeps whatever it needs beyond this, such as the backoff.
   */
  void resetLink(Link& l);
  /** The same for the inbound peer, which has no Link slot. */
  void resetInbound();
  int findByConn(uint16_t conn) const;
  bool enqueue(Link& l, const uint8_t* data, uint16_t len);
  void drain(Link& l, uint8_t idx);
  bool enqueueInbound(const uint8_t* data, uint16_t len);
  void drainInbound();
  void noteAuthFailure(const ble_gap_addr_t& addr);
  /** Forget the inbound peer if its connection has gone. */
  void checkInbound();

  static void connect_cb(uint16_t conn);
  static void disconnect_cb(uint16_t conn, uint8_t reason);
  static void notify_cb(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
  static void written_cb(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len);

  Link _links[MAX_LINKS];
  ble_gap_addr_t _self;
  rx_handler_t _handler = nullptr;
  allow_handler_t _allow = nullptr;
  bool _running = false;
  bool _topology_changed = false;

  ble_gap_addr_t _auth_fail[AUTH_FAIL_DEPTH];
  uint8_t _auth_fail_head = 0, _auth_fail_count = 0;

  /* Inbound peer link. The peripheral role also carries the CLI, so at most one
     slot is left for a peer that dials us. That is enough, because a peer that
     we cannot accept is dialled BY us instead. */
  uint16_t _in_conn = BLE_CONN_HANDLE_INVALID;
  /* The handle we dropped last, and when. See IN_DROP_HOLD_MS. */
  uint16_t _in_dropped_conn = BLE_CONN_HANDLE_INVALID;
  unsigned long _in_dropped_ms = 0;
  uint16_t _in_expect = 0, _in_have = 0;
  uint8_t _in_buf[MAX_FRAME];
  uint8_t _in_hdr[3];
  uint8_t _in_hdr_have = 0;
  bool _in_authed = false;
  unsigned long _in_up_ms = 0;
  /* The same queue for the inbound peer, which we notify rather than write to,
     but which draws on the same TX pool and had the same mid-frame abandonment. */
  uint8_t  _in_txq[TXQ_DEPTH][MAX_FRAME + HDR_SIZE];
  uint16_t _in_txq_len[TXQ_DEPTH];
  uint16_t _in_tx_off = 0;
  uint8_t  _in_txq_head = 0, _in_txq_count = 0;
  uint32_t _tx_dropped_in = 0;
  uint32_t _in_sent = 0;
  unsigned long _in_started_ms = 0;
  unsigned long _in_last_rx_ms = 0;
  uint32_t _in_resyncs = 0;
  uint32_t _in_recv = 0;
};
