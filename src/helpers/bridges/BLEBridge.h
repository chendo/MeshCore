#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BleBridgeFrame.h"
#include "helpers/bridges/BleLink.h"
#include "helpers/bridges/BridgeBase.h"

/* The discovery layer, chosen at build time. BleDiscovery drives the SoftDevice
   advertising set on nRF52; NimBleDiscovery drives the NimBLE advertiser and
   scanner on ESP32. Both present the same beacon on the air. The link backend
   is selected the same way, in BleLink.h. See platformio.ini [bridge]. */
#ifndef BLE_DISCOVERY_HEADER
  #define BLE_DISCOVERY_HEADER "helpers/nrf52/BleDiscovery.h"
#endif
#ifndef BLE_DISCOVERY_CLASS
  #define BLE_DISCOVERY_CLASS BleDiscovery
#endif
#include BLE_DISCOVERY_HEADER

/** Declares the `bridge.secret` CLI setting; see CommonCLI. */
#define BRIDGE_HAS_SECRET 1

/** Peripheral and central slots to request from the SoftDevice.
 *
 *  Peripheral 2: one for the CLI and the DFU port, and one for a peer that
 *  dials IN. The address tie-break means that only the lower address dials, so
 *  the higher address must be able to accept. At 1 slot a CLI session and the
 *  peer link evict each other.
 *  Central 2: one outward link is all that a two-node bridge needs, with one
 *  spare. BleStack spends every slot that we do not ask for on buffers, at
 *  about 1.9KB for each connection. */
#ifndef BLE_PRPH_SLOTS
  #define BLE_PRPH_SLOTS 2
#endif
#ifndef BLE_CENTRAL_SLOTS
  #define BLE_CENTRAL_SLOTS 2
#endif

/**
 * @brief  Carries mesh packets between nRF52 nodes over BLE peer links.
 *
 * The counterpart to ESPNowBridge for a board that cannot reach it. A RAK3401
 * or a ThinkNode M1 has no WiFi, so ESP-NOW is unavailable there. Two
 * co-located nodes that run this bridge with the same `bridge.secret` share
 * their traffic over a BLE connection, and they spend no LoRa hop to do it.
 *
 * Three parts, each in its own file:
 *  - the discovery class starts the BLE stack with the roles that a bridge
 *    needs, advertises a beacon that names our group, and scans for the same
 *    beacon from other nodes.
 *  - BleLink opens the connection and carries the frames, over the transport
 *    backend that the build selected.
 *
 * Both ESP32 and nRF52 nodes run this bridge, and they interoperate: the frame,
 * the beacon record, the group marker, the GATT UUIDs and every timer are the
 * same on both. BLE is the only radio that both families carry, so this is the
 * only transport that joins them.
 *
 * See BleBridgeFrame.h for the wire format, and for why the frame is plaintext
 * with a truncated HMAC rather than obfuscated.
 *
 * TRUST, and where it now happens. The beacon carries a two-byte group marker
 * derived from the secret, and that marker is what stops a node from dialling
 * every MeshCore device in range. The marker is PUBLIC, so it filters and does
 * not authenticate. The proof of group membership is the group HMAC on the
 * FIRST frame over the link.
 *
 * A stranger can therefore make us open a connection, and it can dial us. The
 * rules are the same both ways. It gets no data, and:
 *  - a link whose first frame fails the tag is dropped at once;
 *  - a link that never authenticates is dropped after BleLink::AUTH_GRACE_MS,
 *    whether it wrote something or nothing, and one that authenticates and then
 *    goes silent after BleLink::LINK_IDLE_LIMIT_MS;
 *  - either way the address goes on the deny list for BleDenyList::DENY_MS,
 *    which gates a dial out and an inbound adoption alike.
 *
 * That deny list is load-bearing. There are only three central slots on a
 * RAK3401 (BleLink::MAX_LINKS) and one inbound slot, so slot exhaustion is a
 * real denial of service. A peer that dials in and writes nothing meets the
 * same rules, because BleLink::loop() sweeps the peripheral connections. See
 * BLE_LINK_SILENT_SWEEP for the one build that must switch that sweep off.
 *
 * Configuration:
 *  - put ${bridge.ble} in the env's build_flags, and the .cpp files in its
 *    build_src_filter.
 *  - `bridge.secret` is the shared group key. Set the same value on every node
 *    in the group. A different secret is what keeps two neighbouring groups
 *    apart.
 *
 * This bridge owns the BLE stack on its build. On nRF52 it drives advertising
 * set 0 directly, and on ESP32 it owns the single NimBLE advertiser, so do not
 * compile a BLE CLI or companion interface into the same env. On ESP32 the two
 * host stacks cannot even coexist in one binary. See the discovery class.
 */
class BLEBridge : public BridgeBase {
public:
  BLEBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  /** 0x01 is UART and 0x03 is ESP-NOW, so 0x04 is the first free value. The
   *  code goes out over the air in the repeater status reply, byte 8, where it
   *  shares the byte with the disabled flag. It must stay clear of 0x80. */
  uint8_t getTypeCode() const override { return 0x04; }
  const char *getTypeName() const override { return "ble"; }

  void begin() override;
  void end() override;
  void loop() override;
  void sendPacket(mesh::Packet *packet) override;
  void onPacketReceived(mesh::Packet *packet) override;

  /* Telemetry. The bridge is what a caller has a handle to, so the numbers from
     the layers below come out through here. */
  bool isTransportUp() const { return _transport_up; }

  /** Established peer links, including an inbound one. */
  uint8_t numLinks() const { return _link.numUp(); }
  bool getLinkInfo(uint8_t i, BleAddr& a, bool& up, int8_t& rssi,
                   uint32_t& sent, uint32_t& recv, uint32_t& drops,
                   uint32_t* rx_age_s = nullptr, uint32_t* queued = nullptr) const {
    return _link.getLink(i, a, up, rssi, sent, recv, drops, rx_age_s, queued);
  }
  /** The peer that dialled IN, which holds no outward slot and so appears in no
   *  getLinkInfo() index. */
  bool getInboundInfo(BleAddr& a, uint32_t* rx_age_s = nullptr,
                      uint32_t* queued = nullptr) const {
    return _link.getInboundAddr(a, rx_age_s, queued);
  }

  uint32_t numRxOk() const { return _num_rx_ok; }
  uint32_t numHeartbeatsRx() const { return _num_hb_rx; }
  /**
   * Tag failures on a link.
   *
   * These are NOT expected. The link is connection-oriented and the controller
   * retransmits until delivery or supervision timeout, so bytes do not arrive
   * corrupted. A count above the first frame of a stranger's link means that
   * our own framing is wrong, most likely the reassembly in BleLink, and that
   * is a fault and not noise. The first frame of an unauthenticated link is
   * counted here too, but it also drops the link and denies the address, so
   * numDenied() separates the two causes.
   */
  uint32_t numBadTag() const { return _num_bad_tag; }
  /** Frames on a link that are not our protocol at all. */
  uint32_t numForeign() const { return _num_foreign; }
  uint32_t numSent() const { return _num_sent; }
  /** Packets too large for a frame. See BleBridgeFrame::MAX_PAYLOAD_SIZE: this must
   *  stay 0, and it is counted rather than logged because BRIDGE_DEBUG is off
   *  in every normal build. */
  uint32_t numTxOversize() const { return _codec.numTxOversize(); }
  /** Frames that no link accepted, because no link was up. */
  uint32_t numTxNoLink() const { return _num_tx_no_link; }

  /* Discovery. */
  uint32_t numBeacons() const { return _disc.numBeacons(); }
  uint32_t numForeignBeacons() const { return _disc.numForeignBeacons(); }
  uint32_t reportCount() const { return _disc.reportCount(); }
  uint32_t reportCpuUs() const { return _disc.reportCpuUs(); }
  uint32_t numRecoveries() const { return _disc.numRecoveries(); }
  uint32_t silenceMs() const { return _disc.silenceMs(); }
  uint32_t numAdvFailures() const { return _disc.numAdvFailures(); }
  uint32_t numPresenceAdverts() const { return _disc.numPresenceAdverts(); }
  uint32_t presenceError() const { return _disc.presenceError(); }
  int8_t meanReportRssi() const { return _disc.meanReportRssi(); }

  /* Deny list. */
  uint32_t numDenied() const { return _deny.numAdded(); }
  /** Beacons from a denied address that we did not dial. */
  uint32_t numDialsRefused() const { return _num_dials_refused; }
  /** Peers that dialled IN from a denied address and were disconnected. */
  uint32_t numInboundRefused() const { return _num_inbound_refused; }

  /** The group marker in our beacon, for the CLI. */
  uint16_t groupMarker() const { return _codec.groupMarker(); }

private:
  /**
   * Manufacturer ID that tags our beacon. 0xFFFF is reserved by the Bluetooth
   * SIG for development and for testing, which is what an unregistered open
   * protocol must use.
   */
  static const uint16_t COMPANY_ID = 0xFFFF;

  /** How often a heartbeat goes down every link.
   *
   *  Timed, and not "when idle": a node that bridges occasionally still goes
   *  long enough between packets for a link to be judged dead. At one heartbeat
   *  each 15s against the 60s idle limit in BleLink there are four chances to
   *  miss before that happens. */
  static const uint32_t HEARTBEAT_MS = 15000;

  /** Backoff between attempts to bring the transport up, so a persistent
   *  failure logs occasionally and not once for each loop pass. */
  static const uint32_t START_RETRY_MS = 30000;

  /** How often the battery reading in the beacon is refreshed. */
  static const uint32_t BATTERY_POLL_MS = 30000;

  /** Per-link frame accounting.
   *
   *  A gap in the sequence is the only way to see a frame that arrived zero
   *  times. A gap above SEQ_RESET_GAP is read as a peer that restarted, and its
   *  counter went back to zero, rather than as that many lost frames; without
   *  that the loss figure is poisoned for ever after any reboot. */
  static const uint16_t SEQ_RESET_GAP = 1000;
  struct LinkStamp {
    uint16_t last_seq;
    bool seq_valid;
    uint32_t frames;
    uint32_t lost;
    int32_t skew_s;      // their clock minus ours; a diagnostic only
  };
  /** One for each outward link, plus one for the inbound peer. */
  static const uint8_t NUM_STAMPS = BleLink::MAX_LINKS + 1;

  static BLEBridge *_instance;

  static void link_rx_cb(const uint8_t* data, uint16_t len, uint8_t link_idx);
  static void beacon_cb(const BleAddr& addr, int8_t rssi);
  static bool allow_cb(const BleAddr& addr);
  void onLinkFrame(const uint8_t* data, uint16_t len, uint8_t link_idx);
  void onBeacon(const BleAddr& addr, int8_t rssi);
  /** @returns false to refuse a peer that dialled in. */
  bool onInboundAdopt(const BleAddr& addr);
  void sendHeartbeat();
  /** Map a link index onto a LinkStamp slot. */
  LinkStamp* stampFor(uint8_t link_idx);

  BLE_DISCOVERY_CLASS _disc;
  BleLink _link;
  BleBridgeFrame::FrameCodec _codec;
  BleBridgeFrame::BleDenyList _deny;

  /* Kept off the stack: sendPacket() runs deep inside the dispatcher through
     logTx. _staging is a full MTU because Packet::writeTo() assumes that much
     room and does not bound itself. */
  uint8_t _staging[MAX_TRANS_UNIT];
  uint8_t _tx_frame[BleBridgeFrame::MAX_FRAME];

  bool _transport_up = false;
  unsigned long _next_start_attempt = 0;
  unsigned long _next_hb_ms = 0;
  unsigned long _next_batt_ms = 0;
  uint16_t _tx_seq = 0;

  LinkStamp _stamps[NUM_STAMPS];

  uint32_t _num_rx_ok = 0;
  uint32_t _num_hb_rx = 0;
  uint32_t _num_bad_tag = 0;
  uint32_t _num_foreign = 0;
  uint32_t _num_sent = 0;
  uint32_t _num_tx_no_link = 0;
  uint32_t _num_dials_refused = 0;
  uint32_t _num_inbound_refused = 0;
};
