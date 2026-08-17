#pragma once

#include <stdint.h>
#include <bluefruit.h>

/**
 * @brief  Connection-oriented peer links, as an alternative to broadcasting.
 *
 * Why this exists: the advertising transport loses datagrams and nothing can
 * stop it. Measured on a live pair, a single advert lands about half the time,
 * and the only defence is sending each one three to five times -- which brings
 * loss from 49% to 3.4% but never to zero, because nothing acknowledges
 * anything. Ruling out the alternatives took a while: CPU (2us/report), the
 * BLE central connection (no effect), self-transmission (the node was idle),
 * and foreign advert traffic (86% removed, no improvement) were all excluded
 * by measurement. What is left is intrinsic to extended advertising.
 *
 * A connection is categorically different: the link layer acknowledges every
 * packet and retransmits until it is received or the supervision timeout
 * expires. Delivery is reliable and ordered, and the retries are free.
 *
 * The cost is that a connection is point to point, so this keeps the broadcast
 * transport alongside rather than replacing it -- broadcast remains how peers
 * are discovered, and how a node reaches anyone it has no link to.
 */
class BleLink {
public:
  /** Delivered frames, with the link they arrived on so a caller can avoid
   *  echoing them straight back to their source. */
  typedef void (*rx_handler_t)(const uint8_t* data, uint16_t len, uint8_t link_idx);

  /* Outward links. Bounded by the central slots the SoftDevice granted, which
     on a RAK3401 negotiated to three -- see BleStack. */
  static const uint8_t MAX_LINKS = 3;
  static const uint8_t NO_LINK = 0xFF;

  /* A frame is a bridge datagram: header, packet and tag. */
  static const uint16_t MAX_FRAME = 256;

  /**
   * @param handler    invoked from BLE event context; keep it short.
   * @param self_addr  our own BLE address, for the initiator tie-break below.
   */
  bool begin(rx_handler_t handler, const ble_gap_addr_t& self_addr);

  /** Drive connection attempts, keepalive and retries. Call from the main loop. */
  void loop();

  /** Tear every link down and stop. Disconnects rather than merely forgetting:
   *  a link left connected keeps a peripheral slot occupied on the far side,
   *  and a node with no free peripheral slot stops advertising entirely. */
  void end();

  /** Send to every link regardless of ingress -- used for keepalive traffic. */
  uint8_t sendKeepalive(const uint8_t* data, uint16_t len);

  /**
   * @brief  A peer we have authenticated over broadcast and may connect to.
   *
   * Deliberately fed from the broadcast path rather than from raw scanning: an
   * address that has passed the group HMAC is one we already trust, so nobody
   * can make us dial a stranger.
   */
  void notePeer(const ble_gap_addr_t& addr);

  /**
   * @brief  Send to every established link except one.
   *
   * @param except  link the frame arrived on, or NO_LINK for locally sourced.
   *                Not a loop guard on its own -- the packet-hash tables above
   *                are that -- but it stops the pointless immediate echo.
   * @returns how many links accepted it.
   */
  uint8_t send(const uint8_t* data, uint16_t len, uint8_t except = NO_LINK);

  /** True once since the last call if a link came up or went down -- the caller
   *  must re-arm scanning, which connecting silently stopped. */
  bool takeTopologyChanged() {
    bool c = _topology_changed; _topology_changed = false; return c;
  }

  /** Called by the bridge when a frame from this link passes the group tag --
   *  the only proof that the peer belongs to our bridge. NO_LINK refers to the
   *  inbound peer. */
  void markAuthed(uint8_t idx);

  uint8_t numUp() const;
  bool getLink(uint8_t idx, ble_gap_addr_t& addr, bool& up, int8_t& rssi,
               uint32_t& sent, uint32_t& recv, uint32_t& drops) const;

private:
  /* Only the numerically lower address dials, so a pair converges on exactly
     one link instead of two crossing attempts that each tear the other down.
     No negotiation, no timers, and both ends reach the same answer from
     information they already hold. */
  bool weInitiateTo(const ble_gap_addr_t& peer) const;

  enum State : uint8_t { EMPTY = 0, IDLE, CONNECTING, DISCOVERING, UP };

  /* Framing: [SYNC][len lo][len hi].
     The sync byte exists because the original assumption -- "the link is
     reliable and ordered, so a length prefix is all the framing needed, no gap
     handling, no timeouts" -- fails in the one case that matters.
     The sender used to abandon a frame when a chunk write was refused, having
     ALREADY put the header on the wire, and the receiver had no way to know.
     Without a marker the next frame is appended into the abandoned one
     and the stream desynchronises permanently: every later frame reassembles
     across a boundary, still looks well-formed, and fails its HMAC. That is
     precisely what the split bad-tag counter caught -- 0 on broadcast, climbing
     steadily on the link.
     With a marker the receiver hunts for the next SYNC and recovers on the very
     next frame. */
  static const uint8_t  FRAME_SYNC = 0xA5;
  static const uint8_t  HDR_SIZE = 3;
  /* Frames buffered per destination. Four is roughly a second of bridge
     traffic at the rates observed, which comfortably covers the 20-30ms a
     connection event takes to hand TX credits back. */
  static const uint8_t  TXQ_DEPTH = 4;

  struct Link {
    ble_gap_addr_t addr;
    uint16_t conn;
    State state;
    unsigned long next_try_ms;
    uint16_t backoff_ms;
    uint32_t sent, recv, drops;
    /* A connection can stay nominally up while carrying nothing -- the
       supervision timeout only notices a radio that has gone away, not a peer
       that has stopped talking. Heartbeats give the link a floor of one frame
       per interval, so silence beyond a minute means it is dead and worth
       redialling rather than holding a slot open. */
    unsigned long last_rx_ms;
    /* Reassembly. Writes are capped at the ATT payload -- 20 bytes on the
       default 23-byte MTU -- so a frame arrives in pieces. Raising the MTU
       would cost SoftDevice RAM we do not have, and over a reliable ordered
       link a length prefix is enough. */
    uint16_t rx_expect;
    uint16_t rx_have;
    uint8_t rx_buf[MAX_FRAME];
    /* Header bytes collected so far. A header CAN straddle two writes, and the
       old code simply returned when fewer than two bytes remained, discarding
       them and starting the next write mid-header. */
    uint8_t rx_hdr[3];
    uint8_t rx_hdr_have;
    /* Group membership, proven by one frame passing the bridge's tag check.
       Set via markAuthed() from BLEBridge, which owns the key. */
    bool authed;
    unsigned long up_ms;
    uint32_t unauthed_drops;
    /* When the frame in progress started, for the staleness timeout. */
    unsigned long rx_started_ms;
    uint32_t rx_resyncs;

    /* Outbound queue. send() copies a fully framed datagram in and returns
       immediately; loop() pushes it out as TX credits allow, resuming from
       tx_off wherever the last pass stopped.

       Nothing here blocks, and that is the point. send() is reachable from the
       BLE event task at TASK_PRIO_HIGH, so the earlier retry-with-delay() would
       have stalled a context that preempts both the main loop and the watchdog
       meant to notice a stall. */
    uint8_t  txq[TXQ_DEPTH][MAX_FRAME + HDR_SIZE];
    uint16_t txq_len[TXQ_DEPTH];
    uint16_t tx_off;                     // progress into the head entry
    uint8_t  txq_head, txq_count;
    uint32_t tx_dropped;                 // queue full: honest backpressure
  };

  /* A frame that stops arriving is abandoned rather than waiting forever for
     bytes that will never come. Generous: it only has to exceed the gap between
     frames, not meet any transmission deadline. */
  static const uint32_t RX_STALE_MS = 3000;

  /* How long a link may stay up without a single frame passing the group tag.
     Comfortably longer than the 15s heartbeat, so an ordinary peer is never at
     risk; short enough that a foreign one is not squatting on a slot. */
  static const uint32_t AUTH_GRACE_MS = 45000;

  static const uint32_t LINK_IDLE_LIMIT_MS = 60000;
  static const uint16_t BACKOFF_MIN_MS = 2000;
  static const uint16_t BACKOFF_MAX_MS = 60000;

  void onConnected(uint16_t conn);
  void onDisconnected(uint16_t conn, uint8_t reason);
  void onNotify(uint8_t idx, const uint8_t* data, uint16_t len);
  void onWritten(uint16_t conn, const uint8_t* data, uint16_t len);
  void feed(Link& l, uint8_t idx, const uint8_t* data, uint16_t len);
  /* Shared by the outward links and the inbound peer -- see the .cpp. */
  void reassemble(uint8_t* hdr, uint8_t& hdr_have,
                  uint16_t& expect, uint16_t& have, uint8_t* buf,
                  unsigned long& started_ms, uint32_t& resyncs,
                  uint32_t& recv, unsigned long& last_rx_ms,
                  uint8_t idx, const uint8_t* data, uint16_t len);
  int findByConn(uint16_t conn) const;
  bool enqueue(Link& l, const uint8_t* data, uint16_t len);
  void drain(Link& l, uint8_t idx);
  bool enqueueInbound(const uint8_t* data, uint16_t len);
  void drainInbound();

  static void connect_cb(uint16_t conn);
  static void disconnect_cb(uint16_t conn, uint8_t reason);
  static void notify_cb(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
  static void written_cb(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len);

  Link _links[MAX_LINKS];
  ble_gap_addr_t _self;
  rx_handler_t _handler = nullptr;
  bool _running = false;
  bool _topology_changed = false;

  /* Inbound peer link. The peripheral role also carries the CLI, so at most one
     slot is left for a peer dialling us -- which is fine, because a peer we
     cannot accept will be dialled BY us instead. */
  uint16_t _in_conn = BLE_CONN_HANDLE_INVALID;
  uint16_t _in_expect = 0, _in_have = 0;
  uint8_t _in_buf[MAX_FRAME];
  uint8_t _in_hdr[3];
  uint8_t _in_hdr_have = 0;
  bool _in_authed = false;
  /* Same queue for the inbound peer, which is notified rather than written to
     but draws on the same TX pool and had the same mid-frame abandonment. */
  uint8_t  _in_txq[TXQ_DEPTH][MAX_FRAME + HDR_SIZE];
  uint16_t _in_txq_len[TXQ_DEPTH];
  uint16_t _in_tx_off = 0;
  uint8_t  _in_txq_head = 0, _in_txq_count = 0;
  uint32_t _tx_dropped_in = 0;
  uint32_t _in_recv_sent = 0;
  unsigned long _in_started_ms = 0;
  uint32_t _in_resyncs = 0;
  uint32_t _in_recv = 0;
};
