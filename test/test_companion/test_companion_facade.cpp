// CompanionFacade against a fake BLE link and a fake node.
//
// The frame layouts are covered in test_companion_frames.cpp. These tests cover
// what the facade decides: which command gets which reply, when a login counts,
// which slot a session belongs to, and how a contact list is paced.
//
// THE RULE THAT MATTERS MOST. Each identity keeps its own authority. A login
// against slot 1 must grant nothing on slot 2. A wrong grant here would let a
// room member administer the repeater.

#include <gtest/gtest.h>
#include <helpers/companion/CompanionFacade.h>
#include <string>
#include <vector>

using namespace companion;

namespace {

struct Frame {
  std::vector<uint8_t> bytes;
  uint8_t code() const { return bytes.empty() ? 0 : bytes[0]; }
  size_t size() const { return bytes.size(); }
};

// A BaseSerialInterface that records what the facade sends and replays what a
// test feeds it.
class FakeLink : public BaseSerialInterface {
public:
  bool connected = true;
  bool write_busy = false;
  std::vector<Frame> sent;
  std::vector<Frame> inbox;

  void enable() override {}
  void disable() override {}
  bool isEnabled() const override { return true; }
  bool isConnected() const override { return connected; }
  bool isWriteBusy() const override { return write_busy; }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    Frame f;
    f.bytes.assign(src, src + len);
    sent.push_back(f);
    return len;
  }

  size_t checkRecvFrame(uint8_t dest[]) override {
    if (inbox.empty()) return 0;
    Frame f = inbox.front();
    inbox.erase(inbox.begin());
    memcpy(dest, f.bytes.data(), f.bytes.size());
    return f.bytes.size();
  }

  void feed(const std::vector<uint8_t>& b) {
    Frame f; f.bytes = b; inbox.push_back(f);
  }
  void feedText(const char* s) {
    feed(std::vector<uint8_t>((const uint8_t*)s, (const uint8_t*)s + strlen(s)));
  }
  void clear() { sent.clear(); }
};

struct FakeSlot {
  bool     enabled = false;
  uint8_t  type = COMP_ADV_TYPE_CHAT;
  std::string name;
  std::string admin_pw;
  std::string guest_pw;
  uint32_t lastmod = 0;
  uint8_t  pub_key[PUB_KEY_SIZE];
  // The permissions that the last answered request came in with. The session
  // isolation tests read this.
  int      last_req_perms = -1;
};

class FakeNode : public Host {
public:
  static const int SLOTS = 3;
  FakeSlot slots[SLOTS];
  uint32_t clock = 1766000000;
  bool     clock_settable = true;
  uint16_t millivolts = 4012;
  std::vector<std::string> cli_commands;
  std::string cli_reply;
  int      cli_last_timestamp = -1;

  FakeNode() {
    for (int i = 0; i < SLOTS; i++) {
      memset(slots[i].pub_key, 0, PUB_KEY_SIZE);
      slots[i].pub_key[0] = (uint8_t)(0xA0 + i);
      slots[i].pub_key[1] = (uint8_t)i;
      slots[i].admin_pw = "adm";
      slots[i].guest_pw = "gst";
      slots[i].lastmod = 1000 + i;
      slots[i].name = "slot";
      slots[i].name += (char)('0' + i);
    }
    slots[0].enabled = true;
    slots[0].type = COMP_ADV_TYPE_REPEATER;
  }

  void getDeviceInfo(DeviceInfo& out) override {
    out.firmware_ver_code = 13;
    out.max_contacts_div2 = 8;
    out.max_group_channels = 4;
    out.ble_pin = 123456;
    out.build_date = "14 Aug 2026";
    out.manufacturer = "Fake";
    out.firmware_version = "v1.17.1";
    out.repeat_enabled = 1;
    out.path_hash_mode = 0;
  }

  void getSelfInfo(SelfInfo& out) override {
    memcpy(out.pub_key, slots[0].pub_key, PUB_KEY_SIZE);
    out.adv_type = COMP_ADV_TYPE_CHAT;
    out.tx_power_dbm = 22;
    out.max_tx_power_dbm = 30;
    out.freq_hz = 869525000;
    out.bw_hz = 250000;
    out.sf = 11;
    out.cr = 5;
    strcpy(out.name, "Fake Hydra");
  }

  uint32_t getCurrentTime() override { return clock; }
  bool setCurrentTime(uint32_t secs) override {
    if (!clock_settable || secs < clock) return false;
    clock = secs;
    return true;
  }
  uint16_t getBattMilliVolts() override { return millivolts; }
  void getStorageKb(uint32_t& used, uint32_t& total) override { used = 96; total = 2048; }

  int getSlotCount() override { return SLOTS; }
  int getEnabledSlotCount() override {
    int n = 0;
    for (int i = 0; i < SLOTS; i++) if (slots[i].enabled) n++;
    return n;
  }

  bool getSlotContact(int idx, Contact& out) override {
    if (idx < 0 || idx >= SLOTS || !slots[idx].enabled) return false;
    memset(&out, 0, sizeof(out));
    memcpy(out.pub_key, slots[idx].pub_key, PUB_KEY_SIZE);
    out.type = slots[idx].type;
    out.flags = 0;
    out.out_path_len = 0;
    strncpy(out.name, slots[idx].name.c_str(), sizeof(out.name) - 1);
    out.last_advert = slots[idx].lastmod;
    out.lastmod = slots[idx].lastmod;
    return true;
  }

  int findSlotByPubKey(const uint8_t* pub_key) override {
    for (int i = 0; i < SLOTS; i++) {
      if (!slots[i].enabled) continue;
      if (memcmp(slots[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) return i;
    }
    return -1;
  }

  bool slotLogin(int idx, const char* password, uint8_t& perms) override {
    if (idx < 0 || idx >= SLOTS) return false;
    return loginPermissions(password, slots[idx].admin_pw.c_str(),
                            slots[idx].guest_pw.c_str(), &perms);
  }

  int slotRequest(int idx, uint8_t perms, const uint8_t* req, size_t req_len,
                  uint8_t* dest, size_t max_len) override {
    if (idx < 0 || idx >= SLOTS || req_len < 1) return -1;
    slots[idx].last_req_perms = perms;
    if (req[0] == COMP_REQ_GET_STATUS) {
      Status s;
      memset(&s, 0, sizeof(s));
      s.batt_milli_volts = millivolts;
      s.n_packets_recv = (uint32_t)(100 + idx);   // proves which slot answered
      return (int)encodeStatusBlob(dest, max_len, s);
    }
    if (req[0] == COMP_REQ_GET_ACCESS_LIST) {
      if ((perms & COMP_PERM_ROLE_MASK) != COMP_PERM_ADMIN) return -1;
      dest[0] = 0x05;
      dest[1] = (uint8_t)idx;
      return 2;
    }
    return -1;
  }

  void handleCliCommand(uint32_t sender_timestamp, char* command,
                        char* reply, size_t reply_sz) override {
    cli_commands.push_back(std::string(command));
    cli_last_timestamp = (int)sender_timestamp;
    strncpy(reply, cli_reply.c_str(), reply_sz - 1);
    reply[reply_sz - 1] = 0;
  }
};

struct Rig {
  FakeLink link;
  FakeNode node;
  CompanionFacade facade;
  Rig() : facade(link, node) {}

  // Runs enough passes to drain the inbox and any contact list.
  void pump(int passes = 16) {
    for (int i = 0; i < passes; i++) facade.loop();
  }
  void send(const std::vector<uint8_t>& f) { link.feed(f); facade.loop(); }

  const Frame* firstOf(uint8_t code) const {
    for (const Frame& f : link.sent) if (f.code() == code) return &f;
    return NULL;
  }
  int countOf(uint8_t code) const {
    int n = 0;
    for (const Frame& f : link.sent) if (f.code() == code) n++;
    return n;
  }
};

std::vector<uint8_t> cmd(uint8_t code) { return std::vector<uint8_t>(1, code); }

std::vector<uint8_t> keyCmd(uint8_t code, const uint8_t* pk, const char* tail = NULL) {
  std::vector<uint8_t> f;
  f.push_back(code);
  f.insert(f.end(), pk, pk + PUB_KEY_SIZE);
  if (tail) f.insert(f.end(), (const uint8_t*)tail, (const uint8_t*)tail + strlen(tail));
  return f;
}

}   // namespace

// ------------------------------------------------------- the connection handshake

TEST(Facade, DeviceQueryIsAnswered) {
  Rig r;
  std::vector<uint8_t> f = {COMP_CMD_DEVICE_QUERY, 13};
  r.send(f);
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_DEVICE_INFO, r.link.sent[0].code());
}

TEST(Facade, AppStartAnswersWithSelfInfo) {
  Rig r;
  std::vector<uint8_t> f(8, 0);
  f[0] = COMP_CMD_APP_START;
  r.send(f);
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_SELF_INFO, r.link.sent[0].code());
}

// This is the whole point of the facade. Before it, this exact frame produced a
// text CLI error and the app gave up.
TEST(Facade, AppStartNoLongerFallsThroughToTheCli) {
  Rig r;
  std::vector<uint8_t> f(8, 0);
  f[0] = COMP_CMD_APP_START;
  r.send(f);
  EXPECT_TRUE(r.node.cli_commands.empty());
}

TEST(Facade, GetDeviceTimeReturnsTheClock) {
  Rig r;
  r.send(cmd(COMP_CMD_GET_DEVICE_TIME));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_CURR_TIME, r.link.sent[0].code());
  EXPECT_EQ(r.node.clock, get32(&r.link.sent[0].bytes[1]));
}

TEST(Facade, SetDeviceTimeMovesTheClockForward) {
  Rig r;
  std::vector<uint8_t> f(5);
  f[0] = COMP_CMD_SET_DEVICE_TIME;
  put32(&f[1], 1766000500);
  r.send(f);
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_OK, r.link.sent[0].code());
  EXPECT_EQ(1766000500u, r.node.clock);
}

TEST(Facade, SetDeviceTimeRefusesToRunTheClockBackwards) {
  Rig r;
  std::vector<uint8_t> f(5);
  f[0] = COMP_CMD_SET_DEVICE_TIME;
  put32(&f[1], 1000);
  r.send(f);
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_ERR, r.link.sent[0].code());
  EXPECT_EQ(COMP_ERR_ILLEGAL_ARG, r.link.sent[0].bytes[1]);
}

TEST(Facade, GetBattAndStorageIsAnswered) {
  Rig r;
  r.send(cmd(COMP_CMD_GET_BATT_AND_STORAGE));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_BATT_AND_STORAGE, r.link.sent[0].code());
  EXPECT_EQ(4012u, get16(&r.link.sent[0].bytes[1]));
}

// The app polls this and retries for ever without an answer.
TEST(Facade, SyncNextMessageAlwaysSaysThereIsNothing) {
  Rig r;
  r.send(cmd(COMP_CMD_SYNC_NEXT_MESSAGE));
  r.send(cmd(COMP_CMD_SYNC_NEXT_MESSAGE));
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_NO_MORE_MESSAGES, r.link.sent[0].code());
  EXPECT_EQ(COMP_RESP_NO_MORE_MESSAGES, r.link.sent[1].code());
}

TEST(Facade, ACommandWeDoNotAnswerGetsAClearRefusal) {
  Rig r;
  r.send(cmd(19));    // CMD_REBOOT
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_ERR, r.link.sent[0].code());
  EXPECT_EQ(COMP_ERR_UNSUPPORTED_CMD, r.link.sent[0].bytes[1]);
}

// ------------------------------------------------------------ the contact list

TEST(Facade, GetContactsReportsOneContactForEachEnabledSlot) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  r.pump();

  ASSERT_GE(r.link.sent.size(), 4u);
  EXPECT_EQ(COMP_RESP_CONTACTS_START, r.link.sent[0].code());
  EXPECT_EQ(2u, get32(&r.link.sent[0].bytes[1]));
  EXPECT_EQ(2, r.countOf(COMP_RESP_CONTACT));
  EXPECT_EQ(1, r.countOf(COMP_RESP_END_OF_CONTACTS));
  EXPECT_EQ(COMP_RESP_END_OF_CONTACTS, r.link.sent.back().code());
}

TEST(Facade, ADisabledSlotIsNotAContact) {
  Rig r;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  r.pump();
  EXPECT_EQ(1u, get32(&r.link.sent[0].bytes[1]));
  EXPECT_EQ(1, r.countOf(COMP_RESP_CONTACT));
}

TEST(Facade, TheRepeaterSlotIsReportedAsARepeaterAndAChatSlotAsChat) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  r.pump();
  std::vector<uint8_t> types;
  for (const Frame& f : r.link.sent) {
    if (f.code() == COMP_RESP_CONTACT) types.push_back(f.bytes[1 + PUB_KEY_SIZE]);
  }
  ASSERT_EQ(2u, types.size());
  EXPECT_EQ(COMP_ADV_TYPE_REPEATER, types[0]);
  EXPECT_EQ(COMP_ADV_TYPE_CHAT, types[1]);
}

// The list is paced so that it cannot overrun the 12-frame send queue of
// SerialBLEInterface.
TEST(Facade, OnlyOneContactLeavesForEachPassOfTheLoop) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.node.slots[2].enabled = true;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  EXPECT_EQ(1, r.countOf(COMP_RESP_CONTACTS_START));
  EXPECT_EQ(0, r.countOf(COMP_RESP_CONTACT));
  r.facade.loop();
  EXPECT_EQ(1, r.countOf(COMP_RESP_CONTACT));
  r.facade.loop();
  EXPECT_EQ(2, r.countOf(COMP_RESP_CONTACT));
}

TEST(Facade, AListStopsWhileTheLinkCannotTakeAnotherFrame) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  r.link.write_busy = true;
  r.pump(4);
  EXPECT_EQ(0, r.countOf(COMP_RESP_CONTACT));
  r.link.write_busy = false;
  r.pump();
  EXPECT_EQ(2, r.countOf(COMP_RESP_CONTACT));
}

TEST(Facade, TheSinceFilterSkipsAContactThatDidNotChange) {
  Rig r;
  r.node.slots[1].enabled = true;      // lastmod 1001
  std::vector<uint8_t> f(5);
  f[0] = COMP_CMD_GET_CONTACTS;
  put32(&f[1], 1000);                  // slot 0 has lastmod 1000, so it is skipped
  r.send(f);
  r.pump();
  EXPECT_EQ(1, r.countOf(COMP_RESP_CONTACT));
  // The count in CONTACTS_START is the total, not the filtered count.
  EXPECT_EQ(2u, get32(&r.link.sent[0].bytes[1]));
}

TEST(Facade, EndOfContactsCarriesTheHighestLastmodEvenOfASkippedContact) {
  Rig r;
  r.node.slots[1].enabled = true;
  std::vector<uint8_t> f(5);
  f[0] = COMP_CMD_GET_CONTACTS;
  put32(&f[1], 4000);                  // everything is skipped
  r.send(f);
  r.pump();
  const Frame* end = r.firstOf(COMP_RESP_END_OF_CONTACTS);
  ASSERT_TRUE(end != NULL);
  EXPECT_EQ(1001u, get32(&end->bytes[1]));
}

TEST(Facade, ASecondGetContactsWhileOneIsRunningIsRefused) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.node.slots[2].enabled = true;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  const Frame* err = r.firstOf(COMP_RESP_ERR);
  ASSERT_TRUE(err != NULL);
  EXPECT_EQ(COMP_ERR_BAD_STATE, err->bytes[1]);
}

TEST(Facade, AppStartAbandonsAListThatIsPartWayThrough) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.node.slots[2].enabled = true;
  r.send(cmd(COMP_CMD_GET_CONTACTS));
  std::vector<uint8_t> start(8, 0);
  start[0] = COMP_CMD_APP_START;
  r.send(start);
  r.link.clear();
  r.pump();
  EXPECT_EQ(0, r.countOf(COMP_RESP_CONTACT));
  EXPECT_EQ(0, r.countOf(COMP_RESP_END_OF_CONTACTS));
}

// --------------------------------------------------------------------- login

TEST(Facade, LoginWithTheAdminPasswordSendsSentThenLoginSuccess) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_SENT, r.link.sent[0].code());
  EXPECT_EQ(0, r.link.sent[0].bytes[1]);            // never a flood
  EXPECT_EQ(COMP_PUSH_LOGIN_SUCCESS, r.link.sent[1].code());
  EXPECT_EQ(1, r.link.sent[1].bytes[1]);            // is_admin
  EXPECT_EQ(COMP_PERM_ADMIN, r.link.sent[1].bytes[12]);
  EXPECT_EQ(0, memcmp(&r.link.sent[1].bytes[2], r.node.slots[0].pub_key, 6));
}

TEST(Facade, LoginWithTheGuestPasswordIsNotAdmin) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "gst"));
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_PUSH_LOGIN_SUCCESS, r.link.sent[1].code());
  EXPECT_EQ(0, r.link.sent[1].bytes[1]);
  EXPECT_EQ(COMP_PERM_GUEST, r.link.sent[1].bytes[12]);
}

TEST(Facade, AWrongPasswordGetsLoginFail) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "wrong"));
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_SENT, r.link.sent[0].code());
  EXPECT_EQ(COMP_PUSH_LOGIN_FAIL, r.link.sent[1].code());
}

// The facade has no contact table and no path to a third party.
TEST(Facade, ALoginForAKeyThatIsNotOursIsNotFound) {
  Rig r;
  uint8_t other[PUB_KEY_SIZE];
  memset(other, 0x5A, sizeof(other));
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, other, "adm"));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_ERR, r.link.sent[0].code());
  EXPECT_EQ(COMP_ERR_NOT_FOUND, r.link.sent[0].bytes[1]);
}

TEST(Facade, ALoginForADisabledSlotIsNotFound) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[1].pub_key, "adm"));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_ERR_NOT_FOUND, r.link.sent[0].bytes[1]);
}

// ---------------------------------------------------------------- status request

TEST(Facade, StatusAfterALoginSendsSentThenTheStatusPush) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.link.clear();
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[0].pub_key));
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_SENT, r.link.sent[0].code());
  EXPECT_EQ(COMP_PUSH_STATUS_RESPONSE, r.link.sent[1].code());
  // 1 code + 1 reserved + 6 key prefix + 56 blob. meshcore_py drops anything
  // under 60.
  EXPECT_EQ(64u, r.link.sent[1].size());
  EXPECT_EQ(0, memcmp(&r.link.sent[1].bytes[2], r.node.slots[0].pub_key, 6));
}

TEST(Facade, StatusWithNoLoginIsRefusedAsABadState) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[0].pub_key));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_ERR, r.link.sent[0].code());
  EXPECT_EQ(COMP_ERR_BAD_STATE, r.link.sent[0].bytes[1]);
}

TEST(Facade, StatusForAKeyThatIsNotOursIsNotFound) {
  Rig r;
  uint8_t other[PUB_KEY_SIZE];
  memset(other, 0x11, sizeof(other));
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, other));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_ERR_NOT_FOUND, r.link.sent[0].bytes[1]);
}

// --------------------------------------------------------------- binary request

TEST(Facade, ABinaryRequestAnswersWithATagThatMatchesTheSentFrame) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.link.clear();
  std::vector<uint8_t> f = keyCmd(COMP_CMD_SEND_BINARY_REQ, r.node.slots[0].pub_key);
  f.push_back(COMP_REQ_GET_ACCESS_LIST);
  r.send(f);

  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_SENT, r.link.sent[0].code());
  EXPECT_EQ(COMP_PUSH_BINARY_RESPONSE, r.link.sent[1].code());
  EXPECT_EQ(get32(&r.link.sent[0].bytes[2]), get32(&r.link.sent[1].bytes[2]));
  EXPECT_EQ(0x05, r.link.sent[1].bytes[6]);
}

TEST(Facade, EachRequestGetsItsOwnTag) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  std::vector<uint8_t> f = keyCmd(COMP_CMD_SEND_BINARY_REQ, r.node.slots[0].pub_key);
  f.push_back(COMP_REQ_GET_STATUS);
  r.link.clear();
  r.send(f);
  uint32_t first = get32(&r.link.sent[0].bytes[2]);
  r.link.clear();
  r.send(f);
  uint32_t second = get32(&r.link.sent[0].bytes[2]);
  EXPECT_NE(first, second);
}

TEST(Facade, ARequestTypeTheSlotWillNotAnswerIsRefused) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.link.clear();
  std::vector<uint8_t> f = keyCmd(COMP_CMD_SEND_BINARY_REQ, r.node.slots[0].pub_key);
  f.push_back(COMP_REQ_GET_TELEMETRY_DATA);
  r.send(f);
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_ERR, r.link.sent[0].code());
  EXPECT_EQ(COMP_ERR_UNSUPPORTED_CMD, r.link.sent[0].bytes[1]);
}

// The permission byte reaches the slot, so a slot can refuse a guest.
TEST(Facade, TheSessionPermissionsReachTheSlot) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "gst"));
  r.link.clear();
  std::vector<uint8_t> f = keyCmd(COMP_CMD_SEND_BINARY_REQ, r.node.slots[0].pub_key);
  f.push_back(COMP_REQ_GET_ACCESS_LIST);
  r.send(f);
  EXPECT_EQ(COMP_PERM_GUEST, r.node.slots[0].last_req_perms);
  // The fake slot refuses the ACL to a guest, so the facade reports that.
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_ERR_UNSUPPORTED_CMD, r.link.sent[0].bytes[1]);
}

// ------------------------------------------------------- one session for each slot
//
// The rule that the whole widened scope rests on.

TEST(Session, ALoginOnOneSlotGrantsNothingOnAnother) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[1].pub_key, "adm"));
  r.link.clear();

  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[0].pub_key));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_RESP_ERR, r.link.sent[0].code());
  EXPECT_EQ(COMP_ERR_BAD_STATE, r.link.sent[0].bytes[1]);

  // The slot that WAS logged in still works.
  r.link.clear();
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[1].pub_key));
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ(COMP_PUSH_STATUS_RESPONSE, r.link.sent[1].code());
}

TEST(Session, ARequestIsAnsweredByTheSlotThatTheKeyNames) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[1].pub_key, "adm"));
  r.link.clear();
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[1].pub_key));
  // The fake slot writes 100 + index into n_packets_recv.
  EXPECT_EQ(101u, get32(&r.link.sent[1].bytes[8 + 8]));
}

TEST(Session, AGuestLoginOnOneSlotDoesNotBecomeAdminOnAnother) {
  Rig r;
  r.node.slots[1].enabled = true;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[1].pub_key, "gst"));
  r.link.clear();

  std::vector<uint8_t> f = keyCmd(COMP_CMD_SEND_BINARY_REQ, r.node.slots[1].pub_key);
  f.push_back(COMP_REQ_GET_ACCESS_LIST);
  r.send(f);
  EXPECT_EQ(COMP_PERM_GUEST, r.node.slots[1].last_req_perms);
}

TEST(Session, ALaterLoginWithAWrongPasswordRevokesTheSession) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "wrong"));
  r.link.clear();
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[0].pub_key));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_ERR_BAD_STATE, r.link.sent[0].bytes[1]);
}

TEST(Session, TheSessionEndsWhenTheLinkDrops) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.link.connected = false;
  r.facade.loop();
  r.link.connected = true;
  r.link.clear();
  r.send(keyCmd(COMP_CMD_SEND_STATUS_REQ, r.node.slots[0].pub_key));
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(COMP_ERR_BAD_STATE, r.link.sent[0].bytes[1]);
}

TEST(Session, NothingIsSentWhileTheLinkIsDown) {
  Rig r;
  r.link.connected = false;
  r.link.feed(cmd(COMP_CMD_GET_DEVICE_TIME));
  r.pump();
  EXPECT_TRUE(r.link.sent.empty());
}

// -------------------------------------------------------- the CLI on the same link

TEST(Cli, ATypedLineReachesTheNodeCliAndItsAnswerComesBack) {
  Rig r;
  r.node.cli_reply = "Hilltop";
  r.link.feedText("get name");
  r.facade.loop();
  ASSERT_EQ(1u, r.node.cli_commands.size());
  EXPECT_EQ(std::string("get name"), r.node.cli_commands[0]);
  ASSERT_EQ(1u, r.link.sent.size());
  EXPECT_EQ(std::string("Hilltop"),
            std::string((char*)r.link.sent[0].bytes.data(), r.link.sent[0].size()));
}

TEST(Cli, ALineEndingIsStripped) {
  Rig r;
  r.node.cli_reply = "ok";
  r.link.feedText("advert\r\n");
  r.facade.loop();
  ASSERT_EQ(1u, r.node.cli_commands.size());
  EXPECT_EQ(std::string("advert"), r.node.cli_commands[0]);
}

// A frame that holds only a line ending is ambiguous: 10 is '\n' and is also
// CMD_SYNC_NEXT_MESSAGE. The companion side wins, because the app polls that
// command and a lost poll stops it. See isCompanionFrame().
TEST(Cli, ABareLineEndingGoesToTheCompanionSideNotTheCli) {
  Rig r;
  r.link.feedText("\r\n");
  r.facade.loop();
  EXPECT_TRUE(r.node.cli_commands.empty());
}

TEST(Cli, ALineOfSpacesIsIgnored) {
  Rig r;
  r.link.feedText("   ");
  r.facade.loop();
  EXPECT_TRUE(r.node.cli_commands.empty());
  EXPECT_TRUE(r.link.sent.empty());
}

TEST(Cli, ACommandWithNoAnswerSendsNothing) {
  Rig r;
  r.node.cli_reply = "";
  r.link.feedText("set name x");
  r.facade.loop();
  EXPECT_EQ(1u, r.node.cli_commands.size());
  EXPECT_TRUE(r.link.sent.empty());
}

// A caller over the link is not at the serial console. Upstream uses a
// timestamp that is not 0 to mean that, and commands that report on third
// parties refuse such a caller.
TEST(Cli, TheCallerIsNotTreatedAsTheSerialConsole) {
  Rig r;
  r.node.cli_reply = "ok";
  r.link.feedText("peers");
  r.facade.loop();
  EXPECT_NE(0, r.node.cli_last_timestamp);
}

TEST(Cli, ACompanionFrameNeverReachesTheCli) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  r.send(cmd(COMP_CMD_GET_DEVICE_TIME));
  std::vector<uint8_t> q = {COMP_CMD_DEVICE_QUERY, 13};
  r.send(q);
  EXPECT_TRUE(r.node.cli_commands.empty());
}

// CMD_SEND_BINARY_REQ is the character '2'. It must not land in the CLI.
TEST(Cli, ABinaryRequestFrameNeverReachesTheCli) {
  Rig r;
  r.send(keyCmd(COMP_CMD_SEND_LOGIN, r.node.slots[0].pub_key, "adm"));
  std::vector<uint8_t> f = keyCmd(COMP_CMD_SEND_BINARY_REQ, r.node.slots[0].pub_key);
  f.push_back(COMP_REQ_GET_STATUS);
  r.send(f);
  EXPECT_TRUE(r.node.cli_commands.empty());
}
