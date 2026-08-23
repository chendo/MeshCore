// The companion wire format, byte for byte.
//
// These tests are the only defence against a silent protocol break. The phone
// app is not scriptable in CI, so every offset below was read out of a source
// that the app already agrees with:
//
//   * examples/companion_radio/MyMesh.cpp — the firmware that the app talks to
//   * meshcore.js  src/connection/connection.js — the reference JS client
//   * meshcore_py  src/meshcore/reader.py, parsing.py — the reference Python
//     client, which gates a status push at 60 bytes and reads recv_errors only
//     at 56 bytes of blob
//
// A test here that fails after an edit means the app will stop working.

#include <gtest/gtest.h>
#include <helpers/companion/CompanionFrames.h>
#include <string>
#include <vector>

using namespace companion;

namespace {

std::vector<uint8_t> key(uint8_t first) {
  std::vector<uint8_t> k(PUB_KEY_SIZE);
  for (int i = 0; i < PUB_KEY_SIZE; i++) k[i] = (uint8_t)(first + i);
  return k;
}

Contact sampleContact() {
  Contact c;
  memset(&c, 0, sizeof(c));
  std::vector<uint8_t> k = key(0x40);
  memcpy(c.pub_key, k.data(), PUB_KEY_SIZE);
  c.type = COMP_ADV_TYPE_REPEATER;
  c.flags = 0;
  c.out_path_len = 0;
  strcpy(c.name, "Hilltop");
  c.last_advert = 0x11223344;
  c.gps_lat = -33123456;
  c.gps_lon = 151987654;
  c.lastmod = 0x55667788;
  return c;
}

}   // namespace

// ------------------------------------------------------------- little-endian IO

TEST(Endian, EveryMultiByteFieldIsLittleEndian) {
  uint8_t b[4];
  put16(b, 0x1234);
  EXPECT_EQ(0x34, b[0]);
  EXPECT_EQ(0x12, b[1]);
  put32(b, 0x89ABCDEF);
  EXPECT_EQ(0xEF, b[0]);
  EXPECT_EQ(0xCD, b[1]);
  EXPECT_EQ(0xAB, b[2]);
  EXPECT_EQ(0x89, b[3]);
  EXPECT_EQ(0x1234u, get16((const uint8_t[]){0x34, 0x12}));
  EXPECT_EQ(0x89ABCDEFu, get32(b));
}

TEST(Field, AFixedFieldIsZeroFilledAndAlwaysTerminated) {
  uint8_t b[8];
  memset(b, 0xAA, sizeof(b));
  putField(b, "ab", 8);
  EXPECT_EQ('a', b[0]);
  EXPECT_EQ('b', b[1]);
  for (int i = 2; i < 8; i++) EXPECT_EQ(0, b[i]);

  // A name longer than the field loses its tail, not the terminator.
  memset(b, 0xAA, sizeof(b));
  putField(b, "abcdefghij", 8);
  EXPECT_EQ(0, b[7]);
  EXPECT_EQ(std::string("abcdefg"), std::string((char*)b));
}

// ------------------------------------------------------------ trivial responses

TEST(Encode, Ok) {
  uint8_t b[4];
  ASSERT_EQ(1u, encodeOk(b, sizeof(b)));
  EXPECT_EQ(COMP_RESP_OK, b[0]);
}

TEST(Encode, Err) {
  uint8_t b[4];
  ASSERT_EQ(2u, encodeErr(b, sizeof(b), COMP_ERR_NOT_FOUND));
  EXPECT_EQ(COMP_RESP_ERR, b[0]);
  EXPECT_EQ(COMP_ERR_NOT_FOUND, b[1]);
}

TEST(Encode, NoMoreMessages) {
  uint8_t b[4];
  ASSERT_EQ(1u, encodeNoMoreMessages(b, sizeof(b)));
  EXPECT_EQ(COMP_RESP_NO_MORE_MESSAGES, b[0]);
}

TEST(Encode, CurrTime) {
  uint8_t b[8];
  ASSERT_EQ(5u, encodeCurrTime(b, sizeof(b), 1766000000));
  EXPECT_EQ(COMP_RESP_CURR_TIME, b[0]);
  EXPECT_EQ(1766000000u, get32(&b[1]));
}

TEST(Encode, BattAndStorageCarriesVoltsThenUsedThenTotal) {
  uint8_t b[16];
  ASSERT_EQ(11u, encodeBattAndStorage(b, sizeof(b), 4012, 96, 2048));
  EXPECT_EQ(COMP_RESP_BATT_AND_STORAGE, b[0]);
  EXPECT_EQ(4012u, get16(&b[1]));
  EXPECT_EQ(96u, get32(&b[3]));
  EXPECT_EQ(2048u, get32(&b[7]));
}

TEST(Encode, EveryEncoderRefusesABufferThatIsTooSmall) {
  uint8_t b[4];
  EXPECT_EQ(0u, encodeOk(b, 0));
  EXPECT_EQ(0u, encodeErr(b, 1, 1));
  EXPECT_EQ(0u, encodeCurrTime(b, 4, 1));
  EXPECT_EQ(0u, encodeBattAndStorage(b, 4, 1, 2, 3));
  EXPECT_EQ(0u, encodeContactsStart(b, 4, 1));
  EXPECT_EQ(0u, encodeEndOfContacts(b, 4, 1));
  EXPECT_EQ(0u, encodeSent(b, 4, 0, 1, 2));
  EXPECT_EQ(0u, encodeLoginFail(b, 4, key(1).data()));
  EXPECT_EQ(0u, encodeLoginSuccess(b, 4, key(1).data(), true, 1, 2, 3));
  Contact c = sampleContact();
  EXPECT_EQ(0u, encodeContact(b, 4, COMP_RESP_CONTACT, c));
}

// ------------------------------------------------------------------- contacts

TEST(Encode, ContactsStartCarriesTheTotalCount) {
  uint8_t b[8];
  ASSERT_EQ(5u, encodeContactsStart(b, sizeof(b), 3));
  EXPECT_EQ(COMP_RESP_CONTACTS_START, b[0]);
  EXPECT_EQ(3u, get32(&b[1]));
}

TEST(Encode, EndOfContactsCarriesTheMostRecentLastmod) {
  uint8_t b[8];
  ASSERT_EQ(5u, encodeEndOfContacts(b, sizeof(b), 999));
  EXPECT_EQ(COMP_RESP_END_OF_CONTACTS, b[0]);
  EXPECT_EQ(999u, get32(&b[1]));
}

// The offsets below match onContactResponse() in meshcore.js exactly:
// key 32, type, flags, outPathLen, outPath 64, advName cstring 32,
// lastAdvert u32, advLat i32, advLon i32, lastMod u32.
TEST(Encode, ContactLayoutMatchesTheReferenceClient) {
  uint8_t b[COMP_MAX_FRAME];
  Contact c = sampleContact();
  size_t n = encodeContact(b, sizeof(b), COMP_RESP_CONTACT, c);
  ASSERT_EQ((size_t)COMP_CONTACT_FRAME_LEN, n);
  EXPECT_EQ(148u, n);

  size_t i = 0;
  EXPECT_EQ(COMP_RESP_CONTACT, b[i++]);
  EXPECT_EQ(0, memcmp(&b[i], c.pub_key, PUB_KEY_SIZE)); i += PUB_KEY_SIZE;
  EXPECT_EQ(COMP_ADV_TYPE_REPEATER, b[i++]);
  EXPECT_EQ(0, b[i++]);            // flags
  EXPECT_EQ(0, b[i++]);            // out_path_len
  for (int p = 0; p < MAX_PATH_SIZE; p++) EXPECT_EQ(0, b[i + p]);
  i += MAX_PATH_SIZE;
  EXPECT_EQ(std::string("Hilltop"), std::string((char*)&b[i])); i += 32;
  EXPECT_EQ(0x11223344u, get32(&b[i])); i += 4;
  EXPECT_EQ((int32_t)-33123456, (int32_t)get32(&b[i])); i += 4;
  EXPECT_EQ((int32_t)151987654, (int32_t)get32(&b[i])); i += 4;
  EXPECT_EQ(0x55667788u, get32(&b[i])); i += 4;
  EXPECT_EQ(n, i);
}

TEST(Encode, ContactFitsTheSerialFrame) {
  // BaseSerialInterface caps a frame at 176 bytes. A contact is the largest
  // frame that the facade sends.
  EXPECT_LE(COMP_CONTACT_FRAME_LEN, COMP_MAX_FRAME);
}

TEST(Encode, TheContactCodeIsAParameterSoAPushCanReuseTheLayout) {
  uint8_t b[COMP_MAX_FRAME];
  Contact c = sampleContact();
  ASSERT_GT(encodeContact(b, sizeof(b), 0x8A, c), 0u);
  EXPECT_EQ(0x8A, b[0]);
}

TEST(Encode, AContactNameThatFillsTheFieldStillEndsWithAZero) {
  uint8_t b[COMP_MAX_FRAME];
  Contact c = sampleContact();
  memset(c.name, 'x', sizeof(c.name));
  c.name[sizeof(c.name) - 1] = 0;
  ASSERT_GT(encodeContact(b, sizeof(b), COMP_RESP_CONTACT, c), 0u);
  const uint8_t* name = &b[1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE];
  EXPECT_EQ(0, name[31]);
  EXPECT_EQ(31u, strlen((const char*)name));
}

// -------------------------------------------------------------------- self info

TEST(Encode, SelfInfoLayoutMatchesTheCompanionFirmware) {
  SelfInfo s;
  memset(&s, 0, sizeof(s));
  std::vector<uint8_t> k = key(0x10);
  memcpy(s.pub_key, k.data(), PUB_KEY_SIZE);
  s.adv_type = COMP_ADV_TYPE_CHAT;
  s.tx_power_dbm = 22;
  s.max_tx_power_dbm = 30;
  s.gps_lat = -1;
  s.gps_lon = 2;
  s.multi_acks = 1;
  s.advert_loc_policy = 2;
  s.telemetry_mode = 0x15;
  s.manual_add_contacts = 1;
  s.freq_hz = 869525000;
  s.bw_hz = 250000;
  s.sf = 11;
  s.cr = 5;
  strcpy(s.name, "RAK3401 Hydra");

  uint8_t b[COMP_MAX_FRAME];
  size_t n = encodeSelfInfo(b, sizeof(b), s);
  ASSERT_GT(n, 0u);

  size_t i = 0;
  EXPECT_EQ(COMP_RESP_SELF_INFO, b[i++]);
  EXPECT_EQ(COMP_ADV_TYPE_CHAT, b[i++]);
  EXPECT_EQ(22, (int8_t)b[i++]);
  EXPECT_EQ(30, (int8_t)b[i++]);
  EXPECT_EQ(0, memcmp(&b[i], s.pub_key, PUB_KEY_SIZE)); i += PUB_KEY_SIZE;
  EXPECT_EQ((int32_t)-1, (int32_t)get32(&b[i])); i += 4;
  EXPECT_EQ((int32_t)2, (int32_t)get32(&b[i])); i += 4;
  EXPECT_EQ(1, b[i++]);
  EXPECT_EQ(2, b[i++]);
  EXPECT_EQ(0x15, b[i++]);
  EXPECT_EQ(1, b[i++]);
  EXPECT_EQ(869525000u, get32(&b[i])); i += 4;
  EXPECT_EQ(250000u, get32(&b[i])); i += 4;
  EXPECT_EQ(11, b[i++]);
  EXPECT_EQ(5, b[i++]);
  // The name is the tail of the frame and carries no terminator.
  EXPECT_EQ(std::string("RAK3401 Hydra"), std::string((char*)&b[i], n - i));
  EXPECT_EQ(n, i + strlen("RAK3401 Hydra"));
}

TEST(Encode, SelfInfoClampsAName) {
  SelfInfo s;
  memset(&s, 0, sizeof(s));
  memset(s.name, 'z', sizeof(s.name) - 1);
  uint8_t b[COMP_MAX_FRAME];
  size_t n = encodeSelfInfo(b, sizeof(b), s);
  ASSERT_GT(n, 0u);
  EXPECT_EQ(31u, n - (1 + 3 + PUB_KEY_SIZE + 8 + 4 + 8 + 2));
}

// ------------------------------------------------------------------ device info

TEST(Encode, DeviceInfoLayoutMatchesTheCompanionFirmware) {
  DeviceInfo q;
  memset(&q, 0, sizeof(q));
  q.firmware_ver_code = 13;
  q.max_contacts_div2 = 8;
  q.max_group_channels = 4;
  q.ble_pin = 123456;
  q.build_date = "14 Aug 2026";
  q.manufacturer = "RAK 3401";
  q.firmware_version = "v1.17.1";
  q.repeat_enabled = 1;
  q.path_hash_mode = 0;

  uint8_t b[COMP_MAX_FRAME];
  size_t n = encodeDeviceInfo(b, sizeof(b), q);
  ASSERT_EQ(82u, n);

  size_t i = 0;
  EXPECT_EQ(COMP_RESP_DEVICE_INFO, b[i++]);
  EXPECT_EQ(13, b[i++]);
  EXPECT_EQ(8, b[i++]);
  EXPECT_EQ(4, b[i++]);
  EXPECT_EQ(123456u, get32(&b[i])); i += 4;
  EXPECT_EQ(std::string("14 Aug 2026"), std::string((char*)&b[i])); i += 12;
  EXPECT_EQ(std::string("RAK 3401"), std::string((char*)&b[i])); i += 40;
  EXPECT_EQ(std::string("v1.17.1"), std::string((char*)&b[i])); i += 20;
  EXPECT_EQ(1, b[i++]);
  EXPECT_EQ(0, b[i++]);
  EXPECT_EQ(n, i);
}

TEST(Encode, DeviceInfoSurvivesNullStrings) {
  DeviceInfo q;
  memset(&q, 0, sizeof(q));
  uint8_t b[COMP_MAX_FRAME];
  ASSERT_EQ(82u, encodeDeviceInfo(b, sizeof(b), q));
  EXPECT_EQ(0, b[8]);   // first byte of the build-date field
}

// -------------------------------------------------------------- sent and login

TEST(Encode, SentCarriesTheFloodFlagThenTheTagThenTheTimeout) {
  uint8_t b[16];
  ASSERT_EQ(10u, encodeSent(b, sizeof(b), 0, 0xDEADBEEF, 1000));
  EXPECT_EQ(COMP_RESP_SENT, b[0]);
  EXPECT_EQ(0, b[1]);
  EXPECT_EQ(0xDEADBEEFu, get32(&b[2]));
  EXPECT_EQ(1000u, get32(&b[6]));
}

// meshcore_py reads permissions at byte 1, the key prefix at 2..7, the server
// timestamp at 8..11, the ACL permissions at 12 and the version level at 13.
TEST(Encode, LoginSuccessMatchesTheReferenceClientFieldGates) {
  uint8_t b[32];
  std::vector<uint8_t> k = key(0x70);
  size_t n = encodeLoginSuccess(b, sizeof(b), k.data(), true, 1766000000,
                                COMP_PERM_ADMIN, 2);
  ASSERT_EQ(14u, n);
  EXPECT_EQ(COMP_PUSH_LOGIN_SUCCESS, b[0]);
  EXPECT_EQ(1, b[1]);                       // is_admin
  EXPECT_EQ(0, memcmp(&b[2], k.data(), 6)); // only 6 bytes of the key
  EXPECT_EQ(1766000000u, get32(&b[8]));
  EXPECT_EQ(COMP_PERM_ADMIN, b[12]);
  EXPECT_EQ(2, b[13]);
}

TEST(Encode, LoginSuccessReportsAGuestAsNotAdmin) {
  uint8_t b[32];
  std::vector<uint8_t> k = key(0x70);
  ASSERT_EQ(14u, encodeLoginSuccess(b, sizeof(b), k.data(), false, 1, COMP_PERM_GUEST, 2));
  EXPECT_EQ(0, b[1]);
  EXPECT_EQ(COMP_PERM_GUEST, b[12]);
}

TEST(Encode, LoginFail) {
  uint8_t b[16];
  std::vector<uint8_t> k = key(0x70);
  ASSERT_EQ(8u, encodeLoginFail(b, sizeof(b), k.data()));
  EXPECT_EQ(COMP_PUSH_LOGIN_FAIL, b[0]);
  EXPECT_EQ(0, b[1]);
  EXPECT_EQ(0, memcmp(&b[2], k.data(), 6));
}

// ------------------------------------------------------------------- the status

TEST(Encode, TheStatusBlobIs56BytesInTheOrderOfRepeaterStats) {
  Status s;
  memset(&s, 0, sizeof(s));
  s.batt_milli_volts = 4012;
  s.curr_tx_queue_len = 3;
  s.noise_floor = -110;
  s.last_rssi = -95;
  s.n_packets_recv = 1000;
  s.n_packets_sent = 500;
  s.total_air_time_secs = 60;
  s.total_up_time_secs = 86400;
  s.n_sent_flood = 11;
  s.n_sent_direct = 22;
  s.n_recv_flood = 33;
  s.n_recv_direct = 44;
  s.err_events = 7;
  s.last_snr = -20;     // -5.0 dB, x4
  s.n_direct_dups = 5;
  s.n_flood_dups = 6;
  s.total_rx_air_time_secs = 120;
  s.n_recv_errors = 9;

  uint8_t b[COMP_MAX_FRAME];
  ASSERT_EQ((size_t)COMP_STATUS_LEN, encodeStatusBlob(b, sizeof(b), s));
  EXPECT_EQ(56, COMP_STATUS_LEN);

  EXPECT_EQ(4012u, get16(&b[0]));
  EXPECT_EQ(3u, get16(&b[2]));
  EXPECT_EQ((int16_t)-110, (int16_t)get16(&b[4]));
  EXPECT_EQ((int16_t)-95, (int16_t)get16(&b[6]));
  EXPECT_EQ(1000u, get32(&b[8]));
  EXPECT_EQ(500u, get32(&b[12]));
  EXPECT_EQ(60u, get32(&b[16]));
  EXPECT_EQ(86400u, get32(&b[20]));
  EXPECT_EQ(11u, get32(&b[24]));
  EXPECT_EQ(22u, get32(&b[28]));
  EXPECT_EQ(33u, get32(&b[32]));
  EXPECT_EQ(44u, get32(&b[36]));
  EXPECT_EQ(7u, get16(&b[40]));
  EXPECT_EQ((int16_t)-20, (int16_t)get16(&b[42]));
  EXPECT_EQ(5u, get16(&b[44]));
  EXPECT_EQ(6u, get16(&b[46]));
  EXPECT_EQ(120u, get32(&b[48]));
  EXPECT_EQ(9u, get32(&b[52]));
}

TEST(Encode, TheStatusBlobRefusesASmallBuffer) {
  Status s;
  memset(&s, 0, sizeof(s));
  uint8_t b[55];
  EXPECT_EQ(0u, encodeStatusBlob(b, sizeof(b), s));
}

// meshcore_py drops a status push shorter than 60 bytes and reads recv_errors
// only when the blob reaches 56. 8 + 56 = 64 passes both gates.
TEST(Encode, TheStatusPushIs64BytesAndPassesBothClientGates) {
  Status s;
  memset(&s, 0, sizeof(s));
  uint8_t blob[COMP_STATUS_LEN];
  ASSERT_EQ((size_t)COMP_STATUS_LEN, encodeStatusBlob(blob, sizeof(blob), s));

  uint8_t b[COMP_MAX_FRAME];
  std::vector<uint8_t> k = key(0x30);
  size_t n = encodeStatusResponse(b, sizeof(b), k.data(), blob, sizeof(blob));
  ASSERT_EQ(64u, n);
  EXPECT_GE(n, 60u);
  EXPECT_EQ(COMP_PUSH_STATUS_RESPONSE, b[0]);
  EXPECT_EQ(0, b[1]);
  EXPECT_EQ(0, memcmp(&b[2], k.data(), 6));
  EXPECT_EQ(0, memcmp(&b[8], blob, sizeof(blob)));
}

// The binary response carries a tag and no key. The status push carries a key
// and no tag. That asymmetry is in the firmware; do not "fix" it.
TEST(Encode, BinaryResponseCarriesATagAndNoPublicKey) {
  uint8_t payload[4] = {1, 2, 3, 4};
  uint8_t b[COMP_MAX_FRAME];
  size_t n = encodeBinaryResponse(b, sizeof(b), 0xCAFEBABE, payload, sizeof(payload));
  ASSERT_EQ(10u, n);
  EXPECT_EQ(COMP_PUSH_BINARY_RESPONSE, b[0]);
  EXPECT_EQ(0, b[1]);
  EXPECT_EQ(0xCAFEBABEu, get32(&b[2]));
  EXPECT_EQ(0, memcmp(&b[6], payload, sizeof(payload)));
}

TEST(Encode, TheTwoPushEncodersRefuseAPayloadThatWouldOverrun) {
  uint8_t big[200];
  memset(big, 0, sizeof(big));
  uint8_t b[64];
  std::vector<uint8_t> k = key(1);
  EXPECT_EQ(0u, encodeStatusResponse(b, sizeof(b), k.data(), big, sizeof(big)));
  EXPECT_EQ(0u, encodeBinaryResponse(b, sizeof(b), 1, big, sizeof(big)));
}

// -------------------------------------------------------------------- decoders

TEST(Decode, DeviceQueryGivesTheProtocolVersionOfTheApp) {
  uint8_t f[2] = {COMP_CMD_DEVICE_QUERY, 13};
  uint8_t ver = 0;
  ASSERT_TRUE(decodeDeviceQuery(f, sizeof(f), &ver));
  EXPECT_EQ(13, ver);
  EXPECT_FALSE(decodeDeviceQuery(f, 1, &ver));       // too short
  f[0] = COMP_CMD_APP_START;
  EXPECT_FALSE(decodeDeviceQuery(f, 2, &ver));       // wrong code
}

TEST(Decode, AppStartHasSixReservedBytesThenTheAppName) {
  uint8_t f[16];
  memset(f, 0, sizeof(f));
  f[0] = COMP_CMD_APP_START;
  memcpy(&f[8], "phone", 5);
  char name[16];
  ASSERT_TRUE(decodeAppStart(f, 13, name, sizeof(name)));
  EXPECT_EQ(std::string("phone"), std::string(name));
}

TEST(Decode, AppStartWithNoNameIsStillValid) {
  uint8_t f[8];
  memset(f, 0, sizeof(f));
  f[0] = COMP_CMD_APP_START;
  char name[8];
  ASSERT_TRUE(decodeAppStart(f, 8, name, sizeof(name)));
  EXPECT_EQ(std::string(""), std::string(name));
  EXPECT_FALSE(decodeAppStart(f, 7, name, sizeof(name)));
}

TEST(Decode, AppStartTruncatesANameThatDoesNotFit) {
  uint8_t f[64];
  memset(f, 'q', sizeof(f));
  f[0] = COMP_CMD_APP_START;
  char name[8];
  ASSERT_TRUE(decodeAppStart(f, sizeof(f), name, sizeof(name)));
  EXPECT_EQ(7u, strlen(name));
}

TEST(Decode, SetDeviceTime) {
  uint8_t f[5];
  f[0] = COMP_CMD_SET_DEVICE_TIME;
  put32(&f[1], 1766000000);
  uint32_t secs = 0;
  ASSERT_TRUE(decodeSetDeviceTime(f, sizeof(f), &secs));
  EXPECT_EQ(1766000000u, secs);
  EXPECT_FALSE(decodeSetDeviceTime(f, 4, &secs));
}

TEST(Decode, GetContactsTreatsSinceAsOptional) {
  uint8_t f[5];
  f[0] = COMP_CMD_GET_CONTACTS;
  uint32_t since = 99;
  ASSERT_TRUE(decodeGetContacts(f, 1, &since));
  EXPECT_EQ(0u, since);
  put32(&f[1], 4242);
  ASSERT_TRUE(decodeGetContacts(f, 5, &since));
  EXPECT_EQ(4242u, since);
}

TEST(Decode, SendLoginSplitsTheKeyFromThePassword) {
  uint8_t f[1 + PUB_KEY_SIZE + 8];
  f[0] = COMP_CMD_SEND_LOGIN;
  std::vector<uint8_t> k = key(0x50);
  memcpy(&f[1], k.data(), PUB_KEY_SIZE);
  memcpy(&f[1 + PUB_KEY_SIZE], "hunter2", 7);

  const uint8_t* pk = NULL;
  char pw[16];
  ASSERT_TRUE(decodeSendLogin(f, 1 + PUB_KEY_SIZE + 7, &pk, pw, sizeof(pw)));
  EXPECT_EQ(0, memcmp(pk, k.data(), PUB_KEY_SIZE));
  EXPECT_EQ(std::string("hunter2"), std::string(pw));
}

// The app sends the password with no terminator. A login with a blank password
// is what the app sends for a guest login, and it must parse.
TEST(Decode, SendLoginAcceptsABlankPassword) {
  uint8_t f[1 + PUB_KEY_SIZE];
  memset(f, 0, sizeof(f));
  f[0] = COMP_CMD_SEND_LOGIN;
  const uint8_t* pk = NULL;
  char pw[16];
  memset(pw, 'x', sizeof(pw));
  ASSERT_TRUE(decodeSendLogin(f, sizeof(f), &pk, pw, sizeof(pw)));
  EXPECT_EQ(std::string(""), std::string(pw));
  EXPECT_FALSE(decodeSendLogin(f, sizeof(f) - 1, &pk, pw, sizeof(pw)));
}

TEST(Decode, SendStatusReq) {
  uint8_t f[1 + PUB_KEY_SIZE];
  f[0] = COMP_CMD_SEND_STATUS_REQ;
  std::vector<uint8_t> k = key(0x60);
  memcpy(&f[1], k.data(), PUB_KEY_SIZE);
  const uint8_t* pk = NULL;
  ASSERT_TRUE(decodeSendStatusReq(f, sizeof(f), &pk));
  EXPECT_EQ(0, memcmp(pk, k.data(), PUB_KEY_SIZE));
  EXPECT_FALSE(decodeSendStatusReq(f, sizeof(f) - 1, &pk));
}

TEST(Decode, SendBinaryReqSplitsTheKeyFromTheRequest) {
  uint8_t f[1 + PUB_KEY_SIZE + 4];
  f[0] = COMP_CMD_SEND_BINARY_REQ;
  std::vector<uint8_t> k = key(0x60);
  memcpy(&f[1], k.data(), PUB_KEY_SIZE);
  f[1 + PUB_KEY_SIZE] = COMP_REQ_GET_NEIGHBOURS;
  f[2 + PUB_KEY_SIZE] = 5;

  const uint8_t* pk = NULL;
  const uint8_t* req = NULL;
  size_t req_len = 0;
  ASSERT_TRUE(decodeSendBinaryReq(f, 2 + PUB_KEY_SIZE + 1, &pk, &req, &req_len));
  EXPECT_EQ(0, memcmp(pk, k.data(), PUB_KEY_SIZE));
  EXPECT_EQ(COMP_REQ_GET_NEIGHBOURS, req[0]);
  EXPECT_EQ(2u, req_len);
  // One byte short of a request type is not a binary request.
  EXPECT_FALSE(decodeSendBinaryReq(f, 1 + PUB_KEY_SIZE, &pk, &req, &req_len));
}

// ------------------------------------------------------------- login authority

TEST(Login, TheAdminPasswordGivesAdmin) {
  uint8_t p = 0xFF;
  ASSERT_TRUE(loginPermissions("secret", "secret", "guest", &p));
  EXPECT_EQ(COMP_PERM_ADMIN, p);
}

TEST(Login, TheGuestPasswordGivesGuest) {
  uint8_t p = 0xFF;
  ASSERT_TRUE(loginPermissions("guest", "secret", "guest", &p));
  EXPECT_EQ(COMP_PERM_GUEST, p);
}

TEST(Login, AWrongPasswordFails) {
  uint8_t p = 0xFF;
  EXPECT_FALSE(loginPermissions("nope", "secret", "guest", &p));
  EXPECT_EQ(0xFF, p);
}

TEST(Login, TheAdminPasswordWinsWhenBothAreTheSame) {
  uint8_t p = 0;
  ASSERT_TRUE(loginPermissions("same", "same", "same", &p));
  EXPECT_EQ(COMP_PERM_ADMIN, p);
}

// This is the rule of upstream handleLoginReq(), and it is a trap worth naming:
// a node with no admin password grants ADMIN to a blank login.
TEST(Login, ABlankAdminPasswordMakesABlankLoginAnAdminLogin) {
  uint8_t p = 0;
  ASSERT_TRUE(loginPermissions("", "", "guest", &p));
  EXPECT_EQ(COMP_PERM_ADMIN, p);
}

TEST(Login, ABlankPasswordFailsWhenBothPasswordsAreSet) {
  uint8_t p = 0;
  EXPECT_FALSE(loginPermissions("", "secret", "guest", &p));
}

TEST(Login, ANullPasswordIsTreatedAsBlank) {
  uint8_t p = 0;
  EXPECT_FALSE(loginPermissions(NULL, "secret", "guest", &p));
  ASSERT_TRUE(loginPermissions(NULL, "", "guest", &p));
  EXPECT_EQ(COMP_PERM_ADMIN, p);
}

// ------------------------------------------------------- companion versus CLI
//
// One BLE link carries both. Getting this wrong sends a typed command into the
// frame decoder, or a login frame into the CLI.

TEST(Split, AControlByteIsAlwaysACompanionFrame) {
  uint8_t f[8] = {COMP_CMD_GET_DEVICE_TIME};
  EXPECT_TRUE(isCompanionFrame(f, 1));
  f[0] = COMP_CMD_SYNC_NEXT_MESSAGE;
  EXPECT_TRUE(isCompanionFrame(f, 1));
  f[0] = COMP_CMD_DEVICE_QUERY;
  f[1] = 13;
  EXPECT_TRUE(isCompanionFrame(f, 2));
}

TEST(Split, AnEmptyFrameIsNeither) {
  uint8_t f[1] = {0};
  EXPECT_FALSE(isCompanionFrame(f, 0));
}

TEST(Split, OrdinaryTypedCommandsGoToTheCli) {
  const char* lines[] = {
    "get name", "set name Hilltop", "advert", "clock sync",
    "slot 1 on", "reboot", "password", "2", "22 is not a command"
  };
  for (const char* line : lines) {
    EXPECT_FALSE(isCompanionFrame((const uint8_t*)line, strlen(line))) << line;
  }
}

// CMD_SEND_BINARY_REQ is 50, which is the character '2'. The length test and
// the printable test are what keep it out of the CLI.
TEST(Split, ABinaryRequestIsNotMistakenForATypedLine) {
  uint8_t f[2 + PUB_KEY_SIZE];
  memset(f, 0, sizeof(f));
  f[0] = COMP_CMD_SEND_BINARY_REQ;
  f[1] = 0xAB;     // a key byte that is not printable
  f[1 + PUB_KEY_SIZE] = COMP_REQ_GET_STATUS;
  EXPECT_TRUE(isCompanionFrame(f, sizeof(f)));
}

TEST(Split, AShortLineThatStartsWithTheBinaryRequestCodeIsCli) {
  const char* line = "2";
  EXPECT_FALSE(isCompanionFrame((const uint8_t*)line, 1));
}

// A login frame is 33 bytes and starts with 26, which is a control byte. It
// never reaches the printable test.
TEST(Split, ALoginFrameIsACompanionFrame) {
  uint8_t f[1 + PUB_KEY_SIZE];
  memset(f, 'A', sizeof(f));     // even an all-printable key
  f[0] = COMP_CMD_SEND_LOGIN;
  EXPECT_TRUE(isCompanionFrame(f, sizeof(f)));
}

TEST(Split, ACommandCodeWithTooFewBytesIsNotThatCommand) {
  uint8_t f[4];
  memset(f, 0, sizeof(f));
  f[0] = COMP_CMD_SEND_BINARY_REQ;
  EXPECT_FALSE(isCompanionFrame(f, 4));   // '2' plus three NULs is not printable
}

TEST(Split, AnUnknownControlCodeStillGoesToTheCompanionSide) {
  // The facade answers it with "unsupported command", which is better than
  // feeding it to the CLI as text.
  uint8_t f[2] = {0x0B, 0x00};
  EXPECT_TRUE(isCompanionFrame(f, 2));
}

TEST(Split, AnUnknownPrintableCodeGoesToTheCli) {
  uint8_t f[8] = {'z', 'z', 'z'};
  EXPECT_FALSE(isCompanionFrame(f, 3));
}

TEST(Split, MinCommandLenKnowsOnlyTheCommandsWeAnswer) {
  EXPECT_EQ(8u, minCommandLen(COMP_CMD_APP_START));
  EXPECT_EQ(1u, minCommandLen(COMP_CMD_GET_CONTACTS));
  EXPECT_EQ(2u, minCommandLen(COMP_CMD_DEVICE_QUERY));
  EXPECT_EQ(1u + PUB_KEY_SIZE, minCommandLen(COMP_CMD_SEND_LOGIN));
  EXPECT_EQ(2u + PUB_KEY_SIZE, minCommandLen(COMP_CMD_SEND_BINARY_REQ));
  EXPECT_EQ(0u, minCommandLen(2));    // CMD_SEND_TXT_MSG: not answered
  EXPECT_EQ(0u, minCommandLen(19));   // CMD_REBOOT: not answered
}

TEST(Split, ALineEndingDoesNotMakeALinePrintableOrNot) {
  const char* line = "get name\r\n";
  EXPECT_TRUE(isPrintableRun((const uint8_t*)line, strlen(line)));
  EXPECT_FALSE(isCompanionFrame((const uint8_t*)line, strlen(line)));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
