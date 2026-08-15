#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BridgeBase.h"

#ifdef WITH_BLE_BRIDGE

#include "helpers/nrf52/BleBroadcast.h"

/**
 * @brief Bridge implementation carrying mesh packets over BLE broadcast
 *
 * The nRF52 counterpart to ESPNowBridge. Boards like the RAK3401 have no WiFi,
 * so ESP-NOW is unavailable; BLE 5 extended advertising provides the same
 * connectionless many-to-many broadcast, and every node in range that runs this
 * bridge hears every packet. See BleBroadcast for the transport itself.
 *
 * Packet Structure (inside one Manufacturer Specific Data AD structure):
 *   [1 byte]    version
 *   [4 bytes]   timestamp, little-endian, sender's clock
 *   [<=238]     the mesh packet, IN PLAINTEXT
 *   [8 bytes]   HMAC-SHA256 tag over everything above, truncated
 *
 * Two deliberate differences from ESPNowBridge:
 *
 * No obfuscation. ESPNowBridge XORs its payload with the shared secret, which
 * its own header admits is not encryption. Mesh traffic is already public over
 * the air, so pretending otherwise buys nothing and makes the format harder to
 * implement independently. This one is plaintext and the wire format is meant
 * to be published.
 *
 * Authentication instead of a checksum. Because the format is open, anything
 * in BLE range could otherwise inject packets straight into the mesh. The
 * truncated HMAC replaces BOTH the Fletcher-16 checksum and the XOR: a frame
 * keyed with a different secret fails the tag check, which is precisely the
 * network-isolation role ESPNowBridge's post-encryption checksum plays today.
 *
 * Eight tag bytes put a blind forgery at 2^-64 per attempt, which is out of
 * reach at any rate BLE can offer, and the attacker has to be in radio range to
 * try. For calibration, MeshCore ships two-byte MACs on encrypted direct
 * messages and channel payloads (CIPHER_MAC_SIZE), so this is four times the
 * protocol's existing norm. Truncating the tag does not weaken the key.
 *
 * Configuration:
 * - Define WITH_BLE_BRIDGE to enable this bridge
 * - bridge.secret is the shared group key. It is a credential, not a cipher:
 *   anyone holding it can forge frames, so treat it accordingly and prefer a
 *   random string, since its entropy caps the key's.
 */
class BLEBridge : public BridgeBase {
public:
  /** Frame version. Bump if the layout below ever changes. */
  static const uint8_t FRAME_VERSION = 0x01;

  static const size_t VERSION_SIZE = 1;
  static const size_t TIMESTAMP_SIZE = 4;
  static const size_t TAG_SIZE = 8;
  static const size_t HEADER_SIZE = VERSION_SIZE + TIMESTAMP_SIZE;

  /** What is left for the mesh packet: 251 - 5 - 8 = 238 bytes. ESPNowBridge
   *  manages 246, so this is near parity; a real MeshCore packet is at most
   *  MAX_PACKET_PAYLOAD plus a short path, comfortably inside this. */
  static const size_t MAX_PAYLOAD_SIZE = BleBroadcast::MAX_PAYLOAD - HEADER_SIZE - TAG_SIZE;

  /**
   * @brief Tell the bridge the BLE stack is up, and what to chain events to.
   *
   * The bridge cannot start its transport from begin(): MyMesh::begin() runs it
   * long before main() has called startBLE(), and SerialBLEInterface::begin()
   * would then overwrite the raw event callback we depend on. So the repeater
   * calls this once BLE is initialised and loop() picks it up from there.
   *
   * @param chain  The event callback already installed, if any, so events we do
   *               not consume still reach it. Pass NULL when nothing else on
   *               this build uses BLE -- see BleBroadcast::initStack().
   */
  static void setBleReady(BleBroadcast::event_chain_t chain);

  BLEBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  void begin() override;
  void end() override;
  void loop() override;
  void sendPacket(mesh::Packet *packet) override;
  void onPacketReceived(mesh::Packet *packet) override;

  /** True while a transmission still needs loop() called on time. Lets the
   *  repeater keep sleeping when the bridge is merely listening. */
  bool hasPendingTx() const { return _transport_up && _bcast.hasPendingWork(); }

  /* Telemetry. Every frame carrying our company ID is "seen"; it then lands in
     exactly one of ok / dup / stale / badtag, so the four should sum to seen
     A climbing badtag means another group is in
     range on a different secret -- the mechanism working, not a fault. A large
     dup count is normal and healthy: each datagram is deliberately broadcast
     over several advertising events. */
  bool isTransportUp() const { return _transport_up; }
  uint32_t numSeen() const { return _bcast.numRecv(); }
  uint32_t numRxOk() const { return _num_rx_ok; }
  uint32_t numDup() const { return _num_dup; }
  uint32_t numBadTag() const { return _num_bad_tag; }
  /** Adverts carrying our company ID that are not our protocol at all. 0xFFFF
   *  is the SIG's shared development ID, so other people's beacons land here;
   *  a large count is ambient noise, not a fault. */
  uint32_t numForeign() const { return _num_foreign; }
  uint32_t numSent() const { return _bcast.numSent(); }
  uint32_t numTxDropped() const { return _bcast.numTxDropped(); }

  uint8_t numPeers() const;

  /**
   * @param age_ms  how long since we last accepted a frame from this peer
   * @param skew_s  their clock minus ours, from the last frame's timestamp.
   *                Not acted on -- purely a diagnostic, and the quickest way to
   *                spot a node whose clock has drifted or reset.
   */
  bool getPeer(uint8_t idx, uint8_t addr[6], int8_t &rssi, uint32_t &age_ms,
               uint32_t &frames, int32_t &skew_s) const;

private:
  /**
   * Manufacturer ID tagging our adverts. 0xFFFF is reserved by the Bluetooth
   * SIG for development and testing, which is exactly what an unregistered
   * open protocol should be using.
   */
  static const uint16_t COMPANY_ID = 0xFFFF;

  /** HMAC key size: the full SHA-256 of the configured secret. */
  static const size_t KEY_SIZE = 32;

  /** Senders tracked for replay rejection. A bridge group is a handful of
   *  co-located nodes, so this is generous. */
  static const uint8_t MAX_PEERS = 8;

  /** Backoff between attempts to bring the transport up, so a persistent
   *  failure logs occasionally instead of once per loop iteration. */
  static const uint32_t START_RETRY_MS = 30000;

  struct PeerStamp {
    uint8_t addr[6];
    uint32_t last_timestamp;   // by THEIR clock
    unsigned long last_seen;   // by ours, for staleness and eviction
    uint8_t last_tag[TAG_SIZE];// fingerprint of the last frame accepted
    int8_t last_rssi;          // link quality to this bridge peer
    uint32_t frames;           // accepted from this peer
    bool in_use;
  };

  static BLEBridge *_instance;
  static BleBroadcast::event_chain_t _chain;
  static bool _ble_ready;

  static void rx_cb(const uint8_t *payload, uint8_t len, const uint8_t addr[6], int8_t rssi);
  void onFrameRecv(const uint8_t *payload, uint8_t len, const uint8_t addr[6], int8_t rssi);

  /** Derive the HMAC key from bridge.secret. Called whenever the bridge starts,
   *  so `set bridge.secret` + restartBridge() picks up the new value. */
  void deriveKey();

  /** HMAC-SHA256 over `len` bytes of `frame`, truncated into `tag`. */
  void computeTag(const uint8_t *frame, size_t len, uint8_t tag[TAG_SIZE]);

  /** Record an authenticated frame. Only called once the HMAC has verified. */
  void peerAccept(const uint8_t addr[6], uint32_t timestamp, const uint8_t *tag, int8_t rssi);

  /** Distinguishes a suppressed repeat from a genuine stale frame, so the
   *  telemetry can tell "working as designed" from "something is replaying". */
  bool isDuplicate(const uint8_t addr[6], const uint8_t *tag) const;

  BleBroadcast _bcast;
  uint8_t _key[KEY_SIZE];

  /* Kept off the stack: sendPacket() runs deep inside the dispatcher via
     logTx. _staging is full MTU because Packet::writeTo() assumes that much
     room and does not bound itself. */
  uint8_t _staging[MAX_TRANS_UNIT];
  uint8_t _tx_frame[BleBroadcast::MAX_PAYLOAD];

  bool _transport_up = false;
  unsigned long _next_start_attempt = 0;

  PeerStamp _peers[MAX_PEERS];

  uint32_t _num_bad_tag = 0;
  uint32_t _num_dup = 0;
  uint32_t _num_foreign = 0;
  uint32_t _num_rx_ok = 0;
};

#endif
