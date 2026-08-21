#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <SHA256.h>

/**
 * @brief  The wire format of a BLE bridge frame, and the deny list.
 *
 * Header only, and free of Arduino and of the BLE stack, so the host tests can
 * run it. BLEBridge holds one FrameCodec and one BleDenyList and does the
 * transport work around them.
 *
 * Frame layout, over a BleLink connection:
 *   [1]    version
 *   [2]    sequence, little-endian
 *   [4]    timestamp, little-endian, by the sender's clock
 *   [<=241] the mesh packet, IN PLAINTEXT
 *   [8]    HMAC-SHA256 over everything above, truncated
 *
 * Two deliberate differences from ESPNowBridge:
 *
 * No obfuscation. ESPNowBridge XORs its payload with the shared secret, and its
 * own header admits that this is not encryption. Mesh traffic is already public
 * over the air, so to pretend otherwise buys nothing and makes the format
 * harder to implement independently. This format is plaintext and it is meant
 * to be published.
 *
 * Authentication in place of a checksum. Because the format is open, anything
 * in BLE range could otherwise inject packets straight into the mesh. The
 * truncated HMAC replaces BOTH the Fletcher-16 checksum and the XOR: a frame
 * that carries a different secret fails the tag check. That is exactly the
 * network-isolation role that the post-encryption checksum plays in
 * ESPNowBridge today.
 *
 * Eight tag bytes put a blind forgery at 2^-64 for each attempt, which is out
 * of reach at any rate that BLE offers, and the attacker must be in radio range
 * to try. For comparison, MeshCore ships two-byte MACs on encrypted direct
 * messages and on channel payloads (CIPHER_MAC_SIZE), so this is four times the
 * protocol's own norm. To truncate a tag does not weaken the key.
 *
 * bridge.secret is the shared group key. It is a credential and not a cipher:
 * anyone who holds it can forge frames. Treat it as such, and prefer a random
 * string, because its entropy caps the entropy of the key.
 */
namespace BleBridgeFrame {

/** Frame version. Raise it if the layout above changes.
 *  v2 added the sequence number. The bump was deliberate: a v1 node reads a v2
 *  frame as foreign and ignores it, instead of reading the sequence as part of
 *  the timestamp and acting on nonsense. A mixed-version pair stops to bridge
 *  until both nodes are flashed, which is the honest failure. */
static const uint8_t FRAME_VERSION = 0x02;

/** A frame that carries no packet, sent on a timer.
 *
 *  It gives an idle link a floor of one frame for each interval. Without it a
 *  link with no packets to carry looks the same as a dead one: the supervision
 *  timeout notices a radio that went away, not a peer that stopped to talk. It
 *  is also how a quiet peer proves group membership, because the tag covers a
 *  heartbeat too.
 *
 *  Its own frame type, and not an empty data frame, so a receiver does not
 *  allocate a packet buffer only to fail to parse it. */
static const uint8_t FRAME_HEARTBEAT = 0x03;

static const size_t VERSION_SIZE = 1;
/** Per-sender frame counter, so a receiver can tell a lost frame from a quiet
 *  link. It wraps at 65535, and gaps are computed modulo. */
static const size_t SEQ_SIZE = 2;
static const size_t TIMESTAMP_SIZE = 4;
static const size_t TAG_SIZE = 8;
static const size_t HEADER_SIZE = VERSION_SIZE + SEQ_SIZE + TIMESTAMP_SIZE;   // 7

/** The full SHA-256 of the configured secret is the HMAC key. */
static const size_t KEY_SIZE = 32;

/** The largest frame that BleLink carries. Keep this equal to
 *  BleLink::MAX_FRAME, which this header cannot include because that one needs
 *  the BLE stack. */
static const size_t MAX_FRAME = 256;

/**
 * What is left for the mesh packet: 256 - 7 - 8 = 241 bytes.
 *
 * The advert transport that this replaced carried 251 bytes in an extended
 * advert, which left 236 for a packet. Its own comment claimed 238 and was
 * wrong; the constant was computed and correct. A packet above that limit was
 * discarded in silence, which is why a long path stopped a packet from
 * crossing the bridge.
 *
 * A serialised packet is header(1) + optional transport codes(4) +
 * path length(1) + path + payload, with the payload at most
 * MAX_PACKET_PAYLOAD (184) and the path at most MAX_PATH_SIZE (64). The
 * theoretical maximum is thus 254 bytes, which is above this limit: a packet
 * with a full payload needs a path of 56 hops or more to overflow. Real mesh
 * paths are single digits. The guard in FrameCodec::buildData() therefore
 * stays, and it counts, because a guard that cannot fire must still say so if
 * it does.
 */
static const size_t MAX_PAYLOAD_SIZE = MAX_FRAME - HEADER_SIZE - TAG_SIZE;

/** The label that separates the group marker from the frame tags. The marker
 *  goes out in the clear, so it must not be derived the same way a frame tag
 *  is. */
static const char GROUP_MARKER_LABEL[] = "meshcore-ble-group";

/* ---- The discovery beacon ----------------------------------------------- */

/** Beacon record version, so a later layout change is visible on the air. */
static const uint8_t BEACON_VERSION = 0x01;
/** company(2) + version(1) + marker(2) + battery decivolts(1). */
static const uint8_t BEACON_LEN = 6;

/** What parseBeacon() found in one advert. */
enum BeaconResult : uint8_t {
  BEACON_MATCH = 0,    ///< our company and our group marker: a peer we may dial
  BEACON_FOREIGN,      ///< our company, but a different marker or version
  BEACON_NONE,         ///< no manufacturer data with our company ID
};

/** Fill the manufacturer-data record that goes in the beacon. */
inline void buildBeaconRecord(uint8_t rec[BEACON_LEN], uint16_t company,
                              uint16_t marker, uint8_t batt_dv) {
  rec[0] = (uint8_t)(company & 0xFF);
  rec[1] = (uint8_t)(company >> 8);
  rec[2] = BEACON_VERSION;
  rec[3] = (uint8_t)(marker & 0xFF);
  rec[4] = (uint8_t)(marker >> 8);
  rec[5] = batt_dv;
}

/**
 * @brief  Walk the AD structures of one advert and look for our beacon.
 *
 * Most reports in any populated area are other people's beacons, and they leave
 * at BEACON_NONE well before anything expensive happens.
 *
 * 0xFFFF is the shared development company ID of the Bluetooth SIG, so another
 * person's beacon can carry our company ID. The group marker is what separates
 * a peer of ours from a stranger, and a stranger must not be dialled: there are
 * only three central slots.
 *
 * @param batt_dv  optional: the peer's battery, in decivolts.
 */
inline BeaconResult parseBeacon(const uint8_t* ad, size_t ad_len, uint16_t company,
                                uint16_t marker, uint8_t* batt_dv = nullptr) {
  if (ad == nullptr) return BEACON_NONE;
  const uint8_t* p = ad;
  size_t remaining = ad_len;
  while (remaining >= 2) {
    size_t field_len = p[0];
    if (field_len == 0 || field_len + 1 > remaining) break;      // malformed

    if (p[1] == 0xFF && field_len >= 3) {
      uint16_t co = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
      if (co == company) {
        if (field_len < 1u + BEACON_LEN || p[4] != BEACON_VERSION) return BEACON_FOREIGN;
        uint16_t m = (uint16_t)p[5] | ((uint16_t)p[6] << 8);
        if (m != marker) return BEACON_FOREIGN;
        if (batt_dv) *batt_dv = p[7];
        return BEACON_MATCH;
      }
    }

    p += field_len + 1;
    remaining -= field_len + 1;
  }
  return BEACON_NONE;
}

/* ---- The frame ---------------------------------------------------------- */

/** What parse() found. */
enum ParseResult : uint8_t {
  PARSE_DATA = 0,      ///< a data frame; packet and packet_len are set
  PARSE_HEARTBEAT,     ///< a heartbeat; there is no packet
  PARSE_FOREIGN,       ///< not our protocol at all
  PARSE_BAD_TAG,       ///< our shape, but a different group secret
};

/**
 * @brief  Builds and checks bridge frames with one group key.
 */
class FrameCodec {
public:
  /** Derive the key from bridge.secret.
   *
   *  bridge.secret is a short ASCII string and not key material. To hash it
   *  gives the HMAC a full-width key and stops the length of the secret from
   *  mattering. ESPNowBridge XORs with the raw ASCII, which is the thing that
   *  this repairs. */
  void setSecret(const char* secret) {
    SHA256 sha;
    sha.update((const uint8_t*)secret, strlen(secret));
    sha.finalize(_key, KEY_SIZE);
  }

  /**
   * @brief  Two bytes that name our bridge group, for the discovery beacon.
   *
   * Derived from the key over a fixed label, so it reveals nothing about the
   * key and cannot be confused with a frame tag. It is PUBLIC: an attacker in
   * radio range reads it from any beacon and can repeat it. It thus filters,
   * and it does not authenticate. Its job is to stop a node from dialling every
   * MeshCore device in range, which matters because there are only three
   * central slots.
   */
  uint16_t groupMarker() const {
    uint8_t tag[TAG_SIZE];
    hmac((const uint8_t*)GROUP_MARKER_LABEL, sizeof(GROUP_MARKER_LABEL) - 1, tag);
    return (uint16_t)tag[0] | ((uint16_t)tag[1] << 8);
  }

  /** HMAC-SHA256 over `len` bytes of `frame`, truncated into `tag`. */
  void computeTag(const uint8_t* frame, size_t len, uint8_t tag[TAG_SIZE]) const {
    hmac(frame, len, tag);
  }

  /**
   * @brief  Build a data frame.
   *
   * @param out       at least MAX_FRAME bytes.
   * @param packet    the serialised mesh packet.
   * @returns the frame length, or 0 if the packet is too large. An oversize
   *          packet is COUNTED, because BRIDGE_DEBUG is off in a normal build
   *          and the earlier version recorded nothing at all.
   */
  size_t buildData(uint8_t* out, uint16_t seq, uint32_t timestamp,
                   const uint8_t* packet, size_t packet_len) {
    if (packet_len > MAX_PAYLOAD_SIZE) {
      _num_tx_oversize++;
      return 0;
    }
    out[0] = FRAME_VERSION;
    writeHeader(out, seq, timestamp);
    memcpy(&out[HEADER_SIZE], packet, packet_len);
    const size_t signed_len = HEADER_SIZE + packet_len;
    /* The tag covers the version, the sequence, the timestamp and the packet --
       everything before it -- so no field is malleable. The bytes are
       contiguous, so one HMAC pass covers them. */
    hmac(out, signed_len, &out[signed_len]);
    return signed_len + TAG_SIZE;
  }

  /** Build a heartbeat frame. @returns the frame length. */
  size_t buildHeartbeat(uint8_t* out, uint16_t seq, uint32_t timestamp) const {
    out[0] = FRAME_HEARTBEAT;
    writeHeader(out, seq, timestamp);
    hmac(out, HEADER_SIZE, &out[HEADER_SIZE]);
    return HEADER_SIZE + TAG_SIZE;
  }

  /**
   * @brief  Check a frame and find its packet.
   *
   * Every output parameter may be null. The function writes them only when it
   * returns PARSE_DATA or PARSE_HEARTBEAT.
   */
  ParseResult parse(const uint8_t* frame, size_t len,
                    const uint8_t** packet = nullptr, size_t* packet_len = nullptr,
                    uint16_t* seq = nullptr, uint32_t* timestamp = nullptr) const {
    if (len < HEADER_SIZE + TAG_SIZE) return PARSE_FOREIGN;
    const bool is_hb = (frame[0] == FRAME_HEARTBEAT);
    /* A heartbeat carries no packet, so it is one byte shorter than the
       shortest data frame that we accept. */
    if ((frame[0] != FRAME_VERSION && !is_hb) ||
        len < HEADER_SIZE + TAG_SIZE + (is_hb ? 0u : 1u)) {
      return PARSE_FOREIGN;
    }
    const size_t signed_len = len - TAG_SIZE;
    if (signed_len - HEADER_SIZE > MAX_PAYLOAD_SIZE) return PARSE_FOREIGN;

    uint8_t expected[TAG_SIZE];
    hmac(frame, signed_len, expected);
    if (memcmp(expected, &frame[signed_len], TAG_SIZE) != 0) return PARSE_BAD_TAG;

    if (seq) memcpy(seq, &frame[VERSION_SIZE], SEQ_SIZE);
    if (timestamp) memcpy(timestamp, &frame[VERSION_SIZE + SEQ_SIZE], TIMESTAMP_SIZE);
    if (is_hb) return PARSE_HEARTBEAT;
    if (packet) *packet = &frame[HEADER_SIZE];
    if (packet_len) *packet_len = signed_len - HEADER_SIZE;
    return PARSE_DATA;
  }

  /** Packets that were too large to bridge. This must stay 0. */
  uint32_t numTxOversize() const { return _num_tx_oversize; }

private:
  void writeHeader(uint8_t* out, uint16_t seq, uint32_t timestamp) const {
    memcpy(&out[VERSION_SIZE], &seq, SEQ_SIZE);
    /* The sender's clock, little-endian, to match the advert timestamp in the
       mesh protocol itself. Diagnostic only.
       There is deliberately no freshness check. To reject a frame older than
       the last one from a sender bought almost nothing -- the mesh dedup table
       and MeshCore's own login, admin and advert timestamp checks already catch
       a replay, and the bridge cannot weaken those -- while it cost real
       availability, because these boards have no hardware RTC and any reboot
       moves a clock backwards and got the node ignored for minutes. */
    memcpy(&out[VERSION_SIZE + SEQ_SIZE], &timestamp, TIMESTAMP_SIZE);
  }

  void hmac(const uint8_t* data, size_t len, uint8_t tag[TAG_SIZE]) const {
    SHA256 sha;
    sha.resetHMAC(_key, KEY_SIZE);
    sha.update(data, len);
    sha.finalizeHMAC(_key, KEY_SIZE, tag, TAG_SIZE);
  }

  uint8_t _key[KEY_SIZE] = { 0 };
  uint32_t _num_tx_oversize = 0;
};

/**
 * @brief  Addresses that we refuse to dial for a period.
 *
 * The trust gate moved. The old broadcast transport proved a peer with the
 * group HMAC BEFORE we dialled it, so nobody could choose who we connected to.
 * With broadcast gone, discovery works from a public beacon, and the proof
 * arrives on the first frame over the link instead.
 *
 * A stranger can thus make us open a connection. It gets no data, and we
 * disconnect it. This list is what bounds that: a link that fails the group tag
 * is dropped and its address is refused for DENY_MS. There are only three
 * central slots (BleLink::MAX_LINKS), so slot exhaustion is a real denial of
 * service, and this list is what prevents it.
 *
 * The caller supplies the clock, so the host tests can run it.
 */
class BleDenyList {
public:
  static const uint8_t CAPACITY = 8;
  /** How long an address stays refused. Long enough that a repeat attacker
   *  cannot cycle the slots, and short enough that a peer whose secret was
   *  corrected comes back without a reboot. */
  static const uint32_t DENY_MS = 300000;    // 5 minutes

  /** Refuse this address from now. An address that is already listed has its
   *  period restarted. */
  void add(const uint8_t addr[6], uint32_t now_ms) {
    _num_added++;
    for (uint8_t i = 0; i < CAPACITY; i++) {
      if (_used[i] && memcmp(_addr[i], addr, 6) == 0) {
        _until[i] = now_ms + DENY_MS;
        return;
      }
    }
    /* Take a free slot, or the entry that expires first. The list is full only
       when eight distinct addresses failed inside one period, and to evict the
       oldest keeps the most recent attackers listed. */
    uint8_t victim = 0;
    for (uint8_t i = 0; i < CAPACITY; i++) {
      if (!_used[i]) { victim = i; break; }
      if (_used[victim] && (int32_t)(_until[i] - _until[victim]) < 0) victim = i;
    }
    memcpy(_addr[victim], addr, 6);
    _until[victim] = now_ms + DENY_MS;
    _used[victim] = true;
  }

  /** True while this address is refused. */
  bool isDenied(const uint8_t addr[6], uint32_t now_ms) const {
    for (uint8_t i = 0; i < CAPACITY; i++) {
      if (!_used[i] || memcmp(_addr[i], addr, 6) != 0) continue;
      /* Signed, so the comparison survives the millis() wrap at 49 days. */
      return (int32_t)(now_ms - _until[i]) < 0;
    }
    return false;
  }

  /** How many addresses are refused right now. */
  uint8_t size(uint32_t now_ms) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < CAPACITY; i++) {
      if (_used[i] && (int32_t)(now_ms - _until[i]) < 0) n++;
    }
    return n;
  }

  /** Every add() since boot, including a repeat of one address. */
  uint32_t numAdded() const { return _num_added; }

private:
  uint8_t _addr[CAPACITY][6] = {};
  uint32_t _until[CAPACITY] = {};
  bool _used[CAPACITY] = {};
  uint32_t _num_added = 0;
};

}
