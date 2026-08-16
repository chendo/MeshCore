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

  /** Drive connection attempts and retries. Call from the main loop. */
  void loop();

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

  struct Link {
    ble_gap_addr_t addr;
    uint16_t conn;
    State state;
    unsigned long next_try_ms;
    uint16_t backoff_ms;
    uint32_t sent, recv, drops;
    /* Reassembly. Writes are capped at the ATT payload -- 20 bytes on the
       default 23-byte MTU -- so a frame arrives in pieces. Raising the MTU
       would cost SoftDevice RAM we do not have, and over a reliable ordered
       link a length prefix is enough. */
    uint16_t rx_expect;
    uint16_t rx_have;
    uint8_t rx_buf[MAX_FRAME];
  };

  static const uint16_t BACKOFF_MIN_MS = 2000;
  static const uint16_t BACKOFF_MAX_MS = 60000;

  void onConnected(uint16_t conn);
  void onDisconnected(uint16_t conn, uint8_t reason);
  void onNotify(uint8_t idx, const uint8_t* data, uint16_t len);
  void onWritten(uint16_t conn, const uint8_t* data, uint16_t len);
  void feed(Link& l, uint8_t idx, const uint8_t* data, uint16_t len);
  int findByConn(uint16_t conn) const;
  bool writeFragmented(uint8_t idx, const uint8_t* data, uint16_t len);

  static void connect_cb(uint16_t conn);
  static void disconnect_cb(uint16_t conn, uint8_t reason);
  static void notify_cb(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
  static void written_cb(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len);

  Link _links[MAX_LINKS];
  ble_gap_addr_t _self;
  rx_handler_t _handler = nullptr;
  bool _running = false;

  /* Inbound peer link. The peripheral role also carries the CLI, so at most one
     slot is left for a peer dialling us -- which is fine, because a peer we
     cannot accept will be dialled BY us instead. */
  uint16_t _in_conn = BLE_CONN_HANDLE_INVALID;
  uint16_t _in_expect = 0, _in_have = 0;
  uint8_t _in_buf[MAX_FRAME];
  uint32_t _in_recv = 0;
};
