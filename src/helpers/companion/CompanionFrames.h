#pragma once

// The wire format of the MeshCore companion protocol, as pure serialisation.
//
// The MeshCore phone app speaks this protocol over the Nordic UART service. A
// node that answers it is manageable from the app with no LoRa airtime. This
// header holds only the encode and the decode. It has no Arduino header, no BLE
// header and no mesh state, so the host tests in test/test_companion run the
// same code that the firmware runs.
//
// SOURCE OF THE LAYOUTS. Each layout below comes from
// examples/companion_radio/MyMesh.cpp, which is the firmware that the app talks
// to today. Two reference clients agree with that firmware field for field:
// meshcore.js (src/connection/connection.js) and meshcore_py
// (src/meshcore/reader.py). docs/companion_protocol.md says that it is "still
// in development", so the clients are the authority where they differ.
//
// WHAT THIS HEADER DOES NOT DO. It does not send, receive, or hold a session.
// CompanionFacade does that.

#include <MeshCore.h>   // PUB_KEY_SIZE, MAX_PATH_SIZE. No Arduino, no target headers.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace companion {

// --------------------------------------------------------------- command codes
// The app sends these. Only the codes that the facade answers are here. The
// full set is 1..65; see examples/companion_radio/MyMesh.cpp.
#define COMP_CMD_APP_START            1
#define COMP_CMD_GET_CONTACTS         4
#define COMP_CMD_GET_DEVICE_TIME      5
#define COMP_CMD_SET_DEVICE_TIME      6
#define COMP_CMD_SYNC_NEXT_MESSAGE    10
#define COMP_CMD_GET_BATT_AND_STORAGE 20
#define COMP_CMD_DEVICE_QUERY         22
#define COMP_CMD_SEND_LOGIN           26
#define COMP_CMD_SEND_STATUS_REQ      27
#define COMP_CMD_SEND_BINARY_REQ      50

// -------------------------------------------------------------- response codes
// A reply to the command that came before it.
#define COMP_RESP_OK                  0
#define COMP_RESP_ERR                 1
#define COMP_RESP_CONTACTS_START      2
#define COMP_RESP_CONTACT             3
#define COMP_RESP_END_OF_CONTACTS     4
#define COMP_RESP_SELF_INFO           5
#define COMP_RESP_SENT                6
#define COMP_RESP_CURR_TIME           9
#define COMP_RESP_NO_MORE_MESSAGES    10
#define COMP_RESP_BATT_AND_STORAGE    12
#define COMP_RESP_DEVICE_INFO         13

// ------------------------------------------------------------------ push codes
// The node sends these when it has something to say. They do not answer the
// command that came before them.
#define COMP_PUSH_LOGIN_SUCCESS       0x85
#define COMP_PUSH_LOGIN_FAIL          0x86
#define COMP_PUSH_STATUS_RESPONSE     0x87
#define COMP_PUSH_BINARY_RESPONSE     0x8C

// ----------------------------------------------------------------- error codes
#define COMP_ERR_UNSUPPORTED_CMD      1
#define COMP_ERR_NOT_FOUND            2
#define COMP_ERR_TABLE_FULL           3
#define COMP_ERR_BAD_STATE            4
#define COMP_ERR_FILE_IO_ERROR        5
#define COMP_ERR_ILLEGAL_ARG          6

// ----------------------------------------------------- request types over LoRa
// The first byte of the payload of CMD_SEND_BINARY_REQ. The values match
// examples/simple_repeater/MyMesh.cpp.
#define COMP_REQ_GET_STATUS           0x01
#define COMP_REQ_KEEP_ALIVE           0x02
#define COMP_REQ_GET_TELEMETRY_DATA   0x03
#define COMP_REQ_GET_AVG_MIN_MAX      0x04
#define COMP_REQ_GET_ACCESS_LIST      0x05
#define COMP_REQ_GET_NEIGHBOURS       0x06

// The reply code of a login over LoRa. The facade puts it in a loopback reply.
#define COMP_RESP_SERVER_LOGIN_OK     0

// ------------------------------------------------------------ advert type codes
#define COMP_ADV_TYPE_NONE            0
#define COMP_ADV_TYPE_CHAT            1
#define COMP_ADV_TYPE_REPEATER        2
#define COMP_ADV_TYPE_ROOM            3

// ------------------------------------------------------------- ACL permissions
// The lower 2 bits of the permission byte. They match helpers/ClientACL.h.
#define COMP_PERM_ROLE_MASK           3
#define COMP_PERM_GUEST               0
#define COMP_PERM_READ_ONLY           1
#define COMP_PERM_READ_WRITE          2
#define COMP_PERM_ADMIN               3

// The largest frame that any encoder here writes. A contact frame is the
// largest at 148 bytes. BaseSerialInterface::MAX_FRAME_SIZE is 176.
#define COMP_MAX_FRAME                176

// ------------------------------------------------------------- little-endian IO

inline void put8(uint8_t* d, uint8_t v) { d[0] = v; }

inline void put16(uint8_t* d, uint16_t v) {
  d[0] = (uint8_t)(v & 0xFF);
  d[1] = (uint8_t)(v >> 8);
}

inline void put32(uint8_t* d, uint32_t v) {
  d[0] = (uint8_t)(v & 0xFF);
  d[1] = (uint8_t)((v >> 8) & 0xFF);
  d[2] = (uint8_t)((v >> 16) & 0xFF);
  d[3] = (uint8_t)((v >> 24) & 0xFF);
}

inline uint16_t get16(const uint8_t* d) {
  return (uint16_t)d[0] | ((uint16_t)d[1] << 8);
}

inline uint32_t get32(const uint8_t* d) {
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

// Writes `src` into a fixed field of `field` bytes. It fills the rest of the
// field with zero and always keeps the last byte of the field at zero, so the
// reader gets a terminated string. This is what StrHelper::strzcpy does.
inline void putField(uint8_t* d, const char* src, size_t field) {
  memset(d, 0, field);
  if (src == NULL || field == 0) return;
  size_t n = strlen(src);
  if (n > field - 1) n = field - 1;
  memcpy(d, src, n);
}

// ------------------------------------------------------------------- structures

// One synthetic contact. The facade makes one of these for each identity that
// the node hosts, so that the app can administer each one.
struct Contact {
  uint8_t  pub_key[PUB_KEY_SIZE];
  uint8_t  type;            // COMP_ADV_TYPE_*
  uint8_t  flags;           // bit 0 is 'favourite' in the app; 0 is correct here
  uint8_t  out_path_len;    // 0 means direct with no hop. 0xFF means unknown.
  char     name[32];
  uint32_t last_advert;     // by THEIR clock
  int32_t  gps_lat;         // 6 decimal places
  int32_t  gps_lon;
  uint32_t lastmod;         // by OUR clock. GET_CONTACTS filters on this.
};

// The 56-byte status blob of a repeater. The order and the widths come from
// RepeaterStats in examples/simple_repeater/MyMesh.h. meshcore_py parses it in
// parse_status() and reads recv_errors only when the blob is 56 bytes, so the
// facade always sends all 56.
struct Status {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood;
  uint32_t n_sent_direct;
  uint32_t n_recv_flood;
  uint32_t n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr;        // SNR x 4
  uint16_t n_direct_dups;
  uint16_t n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#define COMP_STATUS_LEN  56

// What the node reports about itself in answer to CMD_APP_START.
struct SelfInfo {
  uint8_t  adv_type;
  int8_t   tx_power_dbm;
  int8_t   max_tx_power_dbm;
  uint8_t  pub_key[PUB_KEY_SIZE];
  int32_t  gps_lat;
  int32_t  gps_lon;
  uint8_t  multi_acks;
  uint8_t  advert_loc_policy;
  uint8_t  telemetry_mode;      // (env << 4) | (loc << 2) | base
  uint8_t  manual_add_contacts;
  uint32_t freq_hz;
  uint32_t bw_hz;
  uint8_t  sf;
  uint8_t  cr;
  char     name[32];
};

// What the node reports about the hardware in answer to CMD_DEVICE_QUERY.
struct DeviceInfo {
  uint8_t     firmware_ver_code;
  uint8_t     max_contacts_div2;
  uint8_t     max_group_channels;
  uint32_t    ble_pin;
  const char* build_date;       // 12-byte field, eg. "14 Aug 2026"
  const char* manufacturer;     // 40-byte field
  const char* firmware_version; // 20-byte field
  uint8_t     repeat_enabled;
  uint8_t     path_hash_mode;
};

// -------------------------------------------------------------------- encoders
//
// Each encoder writes one frame and gives back its length. It gives back 0 if
// the frame does not fit in `max`. The caller must treat 0 as "do not send".

inline size_t encodeOk(uint8_t* d, size_t max) {
  if (max < 1) return 0;
  d[0] = COMP_RESP_OK;
  return 1;
}

inline size_t encodeErr(uint8_t* d, size_t max, uint8_t err_code) {
  if (max < 2) return 0;
  d[0] = COMP_RESP_ERR;
  d[1] = err_code;
  return 2;
}

inline size_t encodeNoMoreMessages(uint8_t* d, size_t max) {
  if (max < 1) return 0;
  d[0] = COMP_RESP_NO_MORE_MESSAGES;
  return 1;
}

inline size_t encodeCurrTime(uint8_t* d, size_t max, uint32_t secs) {
  if (max < 5) return 0;
  d[0] = COMP_RESP_CURR_TIME;
  put32(&d[1], secs);
  return 5;
}

inline size_t encodeBattAndStorage(uint8_t* d, size_t max, uint16_t milli_volts,
                                   uint32_t used_kb, uint32_t total_kb) {
  if (max < 11) return 0;
  d[0] = COMP_RESP_BATT_AND_STORAGE;
  put16(&d[1], milli_volts);
  put32(&d[3], used_kb);
  put32(&d[7], total_kb);
  return 11;
}

inline size_t encodeContactsStart(uint8_t* d, size_t max, uint32_t total_count) {
  if (max < 5) return 0;
  d[0] = COMP_RESP_CONTACTS_START;
  put32(&d[1], total_count);
  return 5;
}

inline size_t encodeEndOfContacts(uint8_t* d, size_t max, uint32_t most_recent_lastmod) {
  if (max < 5) return 0;
  d[0] = COMP_RESP_END_OF_CONTACTS;
  put32(&d[1], most_recent_lastmod);
  return 5;
}

#define COMP_CONTACT_FRAME_LEN  (1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE + 32 + 16)

// `code` is COMP_RESP_CONTACT for a reply to GET_CONTACTS. The same layout
// carries PUSH_CODE_NEW_ADVERT and PUSH_CODE_ADVERT, so the code is a parameter.
// The out_path field is always all zero here: a synthetic contact has no path.
inline size_t encodeContact(uint8_t* d, size_t max, uint8_t code, const Contact& c) {
  if (max < COMP_CONTACT_FRAME_LEN) return 0;
  size_t i = 0;
  d[i++] = code;
  memcpy(&d[i], c.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
  d[i++] = c.type;
  d[i++] = c.flags;
  d[i++] = c.out_path_len;
  memset(&d[i], 0, MAX_PATH_SIZE); i += MAX_PATH_SIZE;
  putField(&d[i], c.name, 32); i += 32;
  put32(&d[i], c.last_advert); i += 4;
  put32(&d[i], (uint32_t)c.gps_lat); i += 4;
  put32(&d[i], (uint32_t)c.gps_lon); i += 4;
  put32(&d[i], c.lastmod); i += 4;
  return i;
}

// The name is the tail of the frame and has no terminator. This is what the
// companion firmware does, and both reference clients read the remainder of the
// frame as the name.
inline size_t encodeSelfInfo(uint8_t* d, size_t max, const SelfInfo& s) {
  size_t name_len = strlen(s.name);
  if (name_len > 31) name_len = 31;
  size_t need = 1 + 3 + PUB_KEY_SIZE + 8 + 4 + 8 + 2 + name_len;
  if (max < need) return 0;
  size_t i = 0;
  d[i++] = COMP_RESP_SELF_INFO;
  d[i++] = s.adv_type;
  d[i++] = (uint8_t)s.tx_power_dbm;
  d[i++] = (uint8_t)s.max_tx_power_dbm;
  memcpy(&d[i], s.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
  put32(&d[i], (uint32_t)s.gps_lat); i += 4;
  put32(&d[i], (uint32_t)s.gps_lon); i += 4;
  d[i++] = s.multi_acks;
  d[i++] = s.advert_loc_policy;
  d[i++] = s.telemetry_mode;
  d[i++] = s.manual_add_contacts;
  put32(&d[i], s.freq_hz); i += 4;
  put32(&d[i], s.bw_hz); i += 4;
  d[i++] = s.sf;
  d[i++] = s.cr;
  memcpy(&d[i], s.name, name_len); i += name_len;
  return i;
}

inline size_t encodeDeviceInfo(uint8_t* d, size_t max, const DeviceInfo& q) {
  const size_t need = 1 + 3 + 4 + 12 + 40 + 20 + 2;
  if (max < need) return 0;
  size_t i = 0;
  d[i++] = COMP_RESP_DEVICE_INFO;
  d[i++] = q.firmware_ver_code;
  d[i++] = q.max_contacts_div2;
  d[i++] = q.max_group_channels;
  put32(&d[i], q.ble_pin); i += 4;
  putField(&d[i], q.build_date, 12); i += 12;
  putField(&d[i], q.manufacturer, 40); i += 40;
  putField(&d[i], q.firmware_version, 20); i += 20;
  d[i++] = q.repeat_enabled;
  d[i++] = q.path_hash_mode;
  return i;
}

// The app matches a later push frame to this one by `tag`. `flood` is 1 when
// the node sent the request by flood. A loopback request never floods, so the
// facade sends 0.
inline size_t encodeSent(uint8_t* d, size_t max, uint8_t flood, uint32_t tag,
                         uint32_t est_timeout_ms) {
  if (max < 10) return 0;
  d[0] = COMP_RESP_SENT;
  d[1] = flood;
  put32(&d[2], tag);
  put32(&d[6], est_timeout_ms);
  return 10;
}

// 14 bytes. meshcore_py reads each trailing field behind its own length gate,
// at 12, 13 and 14 bytes, so a shorter frame is legal but tells the app less.
// The facade always sends the full frame.
inline size_t encodeLoginSuccess(uint8_t* d, size_t max, const uint8_t* pub_key,
                                 bool is_admin, uint32_t server_timestamp,
                                 uint8_t acl_permissions, uint8_t fw_ver_level) {
  if (max < 14) return 0;
  size_t i = 0;
  d[i++] = COMP_PUSH_LOGIN_SUCCESS;
  d[i++] = is_admin ? 1 : 0;
  memcpy(&d[i], pub_key, 6); i += 6;
  put32(&d[i], server_timestamp); i += 4;
  d[i++] = acl_permissions;
  d[i++] = fw_ver_level;
  return i;
}

inline size_t encodeLoginFail(uint8_t* d, size_t max, const uint8_t* pub_key) {
  if (max < 8) return 0;
  d[0] = COMP_PUSH_LOGIN_FAIL;
  d[1] = 0;   // reserved
  memcpy(&d[2], pub_key, 6);
  return 8;
}

// Writes the 56 bytes that a repeater sends after the 4-byte tag of a LoRa
// reply. Both the status push and the binary response carry this blob.
inline size_t encodeStatusBlob(uint8_t* d, size_t max, const Status& s) {
  if (max < COMP_STATUS_LEN) return 0;
  size_t i = 0;
  put16(&d[i], s.batt_milli_volts); i += 2;
  put16(&d[i], s.curr_tx_queue_len); i += 2;
  put16(&d[i], (uint16_t)s.noise_floor); i += 2;
  put16(&d[i], (uint16_t)s.last_rssi); i += 2;
  put32(&d[i], s.n_packets_recv); i += 4;
  put32(&d[i], s.n_packets_sent); i += 4;
  put32(&d[i], s.total_air_time_secs); i += 4;
  put32(&d[i], s.total_up_time_secs); i += 4;
  put32(&d[i], s.n_sent_flood); i += 4;
  put32(&d[i], s.n_sent_direct); i += 4;
  put32(&d[i], s.n_recv_flood); i += 4;
  put32(&d[i], s.n_recv_direct); i += 4;
  put16(&d[i], s.err_events); i += 2;
  put16(&d[i], (uint16_t)s.last_snr); i += 2;
  put16(&d[i], s.n_direct_dups); i += 2;
  put16(&d[i], s.n_flood_dups); i += 2;
  put32(&d[i], s.total_rx_air_time_secs); i += 4;
  put32(&d[i], s.n_recv_errors); i += 4;
  return i;
}

// meshcore_py drops a status push that is shorter than 60 bytes. With the
// 56-byte blob this frame is 64 bytes, so it passes that gate and the
// recv_errors gate as well.
inline size_t encodeStatusResponse(uint8_t* d, size_t max, const uint8_t* pub_key,
                                   const uint8_t* blob, size_t blob_len) {
  if (max < 8 + blob_len) return 0;
  d[0] = COMP_PUSH_STATUS_RESPONSE;
  d[1] = 0;   // reserved
  memcpy(&d[2], pub_key, 6);
  memcpy(&d[8], blob, blob_len);
  return 8 + blob_len;
}

// The app matches this to the tag of the RESP_CODE_SENT frame. Note that this
// frame carries a tag and no public key, and the status push carries a public
// key and no tag. That difference is in the firmware and both clients follow it.
inline size_t encodeBinaryResponse(uint8_t* d, size_t max, uint32_t tag,
                                   const uint8_t* payload, size_t payload_len) {
  if (max < 6 + payload_len) return 0;
  d[0] = COMP_PUSH_BINARY_RESPONSE;
  d[1] = 0;   // reserved
  put32(&d[2], tag);
  memcpy(&d[6], payload, payload_len);
  return 6 + payload_len;
}

// -------------------------------------------------------------------- decoders

// CMD_DEVICE_QUERY carries the protocol version that the app understands.
inline bool decodeDeviceQuery(const uint8_t* f, size_t len, uint8_t* app_ver) {
  if (len < 2 || f[0] != COMP_CMD_DEVICE_QUERY) return false;
  *app_ver = f[1];
  return true;
}

// CMD_APP_START is [code][6 reserved][app name]. The name has no terminator, so
// the decoder copies it into a buffer that it terminates.
inline bool decodeAppStart(const uint8_t* f, size_t len, char* app_name, size_t name_sz) {
  if (len < 8 || f[0] != COMP_CMD_APP_START) return false;
  if (app_name != NULL && name_sz > 0) {
    size_t n = len - 8;
    if (n > name_sz - 1) n = name_sz - 1;
    memcpy(app_name, &f[8], n);
    app_name[n] = 0;
  }
  return true;
}

inline bool decodeSetDeviceTime(const uint8_t* f, size_t len, uint32_t* secs) {
  if (len < 5 || f[0] != COMP_CMD_SET_DEVICE_TIME) return false;
  *secs = get32(&f[1]);
  return true;
}

// The 'since' parameter is optional. Without it the app wants every contact.
inline bool decodeGetContacts(const uint8_t* f, size_t len, uint32_t* since) {
  if (len < 1 || f[0] != COMP_CMD_GET_CONTACTS) return false;
  *since = (len >= 5) ? get32(&f[1]) : 0;
  return true;
}

// CMD_SEND_LOGIN is [26][pub_key 32][password]. The password has no terminator
// in the frame.
inline bool decodeSendLogin(const uint8_t* f, size_t len, const uint8_t** pub_key,
                            char* password, size_t pw_sz) {
  if (len < 1 + PUB_KEY_SIZE || f[0] != COMP_CMD_SEND_LOGIN) return false;
  *pub_key = &f[1];
  if (password != NULL && pw_sz > 0) {
    size_t n = len - (1 + PUB_KEY_SIZE);
    if (n > pw_sz - 1) n = pw_sz - 1;
    memcpy(password, &f[1 + PUB_KEY_SIZE], n);
    password[n] = 0;
  }
  return true;
}

inline bool decodeSendStatusReq(const uint8_t* f, size_t len, const uint8_t** pub_key) {
  if (len < 1 + PUB_KEY_SIZE || f[0] != COMP_CMD_SEND_STATUS_REQ) return false;
  *pub_key = &f[1];
  return true;
}

// CMD_SEND_BINARY_REQ is [50][pub_key 32][request]. The first byte of the
// request is a COMP_REQ_* type.
inline bool decodeSendBinaryReq(const uint8_t* f, size_t len, const uint8_t** pub_key,
                                const uint8_t** req, size_t* req_len) {
  if (len < 2 + PUB_KEY_SIZE || f[0] != COMP_CMD_SEND_BINARY_REQ) return false;
  *pub_key = &f[1];
  *req = &f[1 + PUB_KEY_SIZE];
  *req_len = len - (1 + PUB_KEY_SIZE);
  return true;
}

// ------------------------------------------------------------- login authority
//
// The rule of examples/simple_repeater/MyMesh.cpp handleLoginReq(): the admin
// password wins, then the guest password, and anything else fails. Note that an
// empty admin password makes an empty login an ADMIN login. That is the rule
// upstream applies, so the facade applies it too. A node that must not do this
// has to set a password.

inline bool loginPermissions(const char* password, const char* admin_pw,
                             const char* guest_pw, uint8_t* perms) {
  if (password == NULL) password = "";
  if (admin_pw != NULL && strcmp(password, admin_pw) == 0) {
    *perms = COMP_PERM_ADMIN;
    return true;
  }
  if (guest_pw != NULL && strcmp(password, guest_pw) == 0) {
    *perms = COMP_PERM_GUEST;
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ frame split
//
// One BLE connection carries both the companion protocol and the text CLI. The
// node must tell them apart with no framing of its own.
//
// A CLI line is printable ASCII. A companion frame starts with a command code.
// Most codes are below 0x20, which no printable line can start with. Three of
// the codes that the facade answers are not below 0x20, and CMD_SEND_BINARY_REQ
// (50) is the character '2'. So a second test is needed: a companion frame with
// a 32-byte public key in it is almost never printable from end to end.
//
// The residual ambiguity is a binary request whose public key and body are all
// printable. That is about (95/256)^33, or 1 in 10^14. A CLI line of 34
// characters or more that starts with '2' is read as a command, which is
// correct, because it is one.
//
// ONE AMBIGUITY IS REAL AND THE RULE RESOLVES IT DELIBERATELY. A frame that
// holds only a line ending is both CMD_SYNC_NEXT_MESSAGE (10, which is '\n')
// or CMD_RESET_PATH (13, which is '\r'), and the Enter key of a person at a
// terminal. The companion side wins, because the app polls SYNC_NEXT_MESSAGE
// for as long as it is connected and a lost poll stops the app. An Enter on an
// empty line then produces one "no more messages" frame, which costs nothing.

inline bool isPrintableRun(const uint8_t* f, size_t len) {
  for (size_t i = 0; i < len; i++) {
    // Tab, carriage return and newline can end a typed line.
    if (f[i] == '\t' || f[i] == '\r' || f[i] == '\n') continue;
    if (f[i] < 0x20 || f[i] > 0x7E) return false;
  }
  return true;
}

// The shortest frame that each answered command can be. A frame that is shorter
// is not that command.
inline size_t minCommandLen(uint8_t code) {
  switch (code) {
    case COMP_CMD_APP_START:            return 8;
    case COMP_CMD_GET_CONTACTS:         return 1;
    case COMP_CMD_GET_DEVICE_TIME:      return 1;
    case COMP_CMD_SET_DEVICE_TIME:      return 5;
    case COMP_CMD_SYNC_NEXT_MESSAGE:    return 1;
    case COMP_CMD_GET_BATT_AND_STORAGE: return 1;
    case COMP_CMD_DEVICE_QUERY:         return 2;
    case COMP_CMD_SEND_LOGIN:           return 1 + PUB_KEY_SIZE;
    case COMP_CMD_SEND_STATUS_REQ:      return 1 + PUB_KEY_SIZE;
    case COMP_CMD_SEND_BINARY_REQ:      return 2 + PUB_KEY_SIZE;
    default:                            return 0;   // not a command that we answer
  }
}

inline bool isCompanionFrame(const uint8_t* f, size_t len) {
  if (len < 1) return false;
  size_t need = minCommandLen(f[0]);
  if (need == 0) {
    // Not a command we answer. A control byte still cannot start a CLI line, so
    // it belongs to the companion side and gets an "unsupported" reply there.
    return f[0] < 0x20;
  }
  if (len < need) return false;
  if (f[0] < 0x20) return true;
  return !isPrintableRun(f, len);
}

}   // namespace companion
