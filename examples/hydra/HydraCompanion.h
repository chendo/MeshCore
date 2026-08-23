#pragma once

// Joins the BLE companion facade to a hydra node.
//
// The facade in src/helpers/companion is node-agnostic. This file is the only
// place that knows what a hydra slot is. It reports one synthetic contact for
// each enabled slot, so that the phone app lists every identity that this board
// hosts and can open the repeater screens on the one it names.
//
// WHAT AN OPERATOR GETS. Stand beside the node, open the app, connect over BLE
// and the repeater is in the contact list. Log in, read the status, read the
// ACL. No packet goes on the air for any of it. The same link also carries the
// text CLI, which reaches every slot through `slot N <command>`.
//
// WHAT IS NOT HERE YET. Only the repeater slot has an ACL and a status. A chat
// slot has neither, so it appears in the list and refuses a login. When the
// room protocol lands, a room slot gets its own ACL and its case goes in
// slotLogin() and slotRequest() below. Nothing else has to change.

#include "HydraNode.h"
#include <helpers/companion/CompanionFacade.h>
#include <target.h>   // board, radio_driver, rtc_clock

// Which version of the companion protocol this node claims. The app negotiates
// on it. 13 is what examples/companion_radio reports, so the app takes its
// newest path and every command that this facade does not answer gets a clean
// "unsupported" reply. Lower it if a client turns out to need that.
#ifndef COMPANION_FACADE_VER_CODE
  #define COMPANION_FACADE_VER_CODE  13
#endif

#ifndef BLE_PIN_CODE
  #define BLE_PIN_CODE  123456
#endif

// Only the companion variants set this, so a repeater board can leave it unset.
#ifndef MAX_LORA_TX_POWER
  #define MAX_LORA_TX_POWER  LORA_TX_POWER
#endif

class HydraCompanionHost : public companion::Host {
  /* WHEN A CONTACT LAST CHANGED. The app syncs contacts with a 'since'
     timestamp, so each contact needs a lastmod that moves only when something
     about it moves. The node has no such record, so this class keeps one: it
     watches the tuple (enabled, type, name) of each slot and stamps the clock
     when that tuple changes. A stamp of 0 means "seen for the first time", and
     the first pass writes the clock into it. */
  uint32_t _lastmod[HYDRA_NUM_SLOTS];
  uint32_t _sig[HYDRA_NUM_SLOTS];

  static uint32_t signatureOf(bool live, uint8_t type, const char* name) {
    uint32_t h = live ? 0x9E3779B9u : 0x12345678u;
    h = h * 33u + type;
    for (const char* p = name; p && *p; p++) h = h * 33u + (uint8_t)*p;
    return h;
  }

  bool slotLive(int idx) const {
    return idx >= 0 && idx < HYDRA_NUM_SLOTS && hydra.radio().portActive(idx);
  }

  NodePrefs* prefs0() { return hydra.repeater().prefs(); }

public:
  HydraCompanionHost() {
    memset(_lastmod, 0, sizeof(_lastmod));
    memset(_sig, 0, sizeof(_sig));
  }

  // ------------------------------------------------------------------ the node

  void getDeviceInfo(companion::DeviceInfo& out) override {
    out.firmware_ver_code = COMPANION_FACADE_VER_CODE;
    out.max_contacts_div2 = (HYDRA_NUM_SLOTS + 1) / 2;
    out.max_group_channels = 0;   // this facade holds no group channels
    out.ble_pin = BLE_PIN_CODE;
    out.build_date = FIRMWARE_BUILD_DATE;
    out.manufacturer = board.getManufacturerName();
    out.firmware_version = FIRMWARE_VERSION;
    out.repeat_enabled = prefs0()->disable_fwd ? 0 : 1;
    out.path_hash_mode = prefs0()->path_hash_mode;
  }

  /* The identity that the app sees as "the radio I am attached to". It is slot
     0, and slot 0 is ALSO in the contact list, because that is the identity an
     operator wants to administer. The type is CHAT, which is what a real
     companion reports; the contact entry for the same key reports REPEATER. */
  void getSelfInfo(companion::SelfInfo& out) override {
    NodePrefs* p = prefs0();
    memcpy(out.pub_key, hydra.repeater().identity().pub_key, PUB_KEY_SIZE);
    out.adv_type = COMP_ADV_TYPE_CHAT;
    out.tx_power_dbm = p->tx_power_dbm;
    out.max_tx_power_dbm = MAX_LORA_TX_POWER;
    out.gps_lat = (int32_t)(p->node_lat * 1000000.0);
    out.gps_lon = (int32_t)(p->node_lon * 1000000.0);
    out.multi_acks = p->multi_acks;
    out.advert_loc_policy = p->advert_loc_policy;
    out.telemetry_mode = 0;
    out.manual_add_contacts = 1;   // this node adds nothing to a contact list
    out.freq_hz = (uint32_t)(p->freq * 1000.0f);
    out.bw_hz = (uint32_t)(p->bw * 1000.0f);
    out.sf = p->sf;
    out.cr = p->cr;
    StrHelper::strzcpy(out.name, p->node_name, sizeof(out.name));
  }

  uint32_t getCurrentTime() override { return rtc_clock.getCurrentTime(); }

  bool setCurrentTime(uint32_t secs) override {
    // Upstream refuses a time that runs the clock backwards. Keep that rule:
    // a rewound clock makes every advert of this node look like a replay.
    if (secs < rtc_clock.getCurrentTime()) return false;
    rtc_clock.setCurrentTime(secs);
    return true;
  }

  uint16_t getBattMilliVolts() override { return board.getBattMilliVolts(); }

  // The filesystem of this node reports no usage figure. The app shows 0, which
  // is honest. Fill this in when a node gains a store that can measure itself.
  void getStorageKb(uint32_t& used, uint32_t& total) override { used = 0; total = 0; }

  // ------------------------------------------------------------ the identities

  int getSlotCount() override { return HYDRA_NUM_SLOTS; }

  int getEnabledSlotCount() override {
    int n = 0;
    for (int i = 0; i < HYDRA_NUM_SLOTS; i++) if (slotLive(i)) n++;
    return n;
  }

  bool getSlotContact(int idx, companion::Contact& out) override {
    if (!slotLive(idx)) return false;
    HydraSlot* s = hydra.slot(idx);
    if (s == NULL) return false;

    const char* nm = s->name();
    uint8_t adv_type;
    switch (s->type()) {
      case SLOT_REPEATER: adv_type = COMP_ADV_TYPE_REPEATER; break;
      case SLOT_ROOM:     adv_type = COMP_ADV_TYPE_ROOM; break;
      default:            adv_type = COMP_ADV_TYPE_CHAT; break;
    }

    uint32_t sig = signatureOf(true, adv_type, nm);
    if (sig != _sig[idx] || _lastmod[idx] == 0) {
      _sig[idx] = sig;
      _lastmod[idx] = rtc_clock.getCurrentTime();
    }

    memset(&out, 0, sizeof(out));
    memcpy(out.pub_key, s->identity().pub_key, PUB_KEY_SIZE);
    out.type = adv_type;
    out.flags = 0;
    // 0 is "direct, no hop". The app must not try to flood to reach it, and it
    // never leaves this board in any case.
    out.out_path_len = 0;
    StrHelper::strzcpy(out.name, nm, sizeof(out.name));
    out.last_advert = _lastmod[idx];
    out.gps_lat = 0;
    out.gps_lon = 0;
    out.lastmod = _lastmod[idx];
    return true;
  }

  int findSlotByPubKey(const uint8_t* pub_key) override {
    for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
      if (!slotLive(i)) continue;
      HydraSlot* s = hydra.slot(i);
      if (s && memcmp(s->identity().pub_key, pub_key, PUB_KEY_SIZE) == 0) return i;
    }
    return -1;
  }

  /* Each identity holds its own passwords, so a login authorises that identity
     and no other. Only the repeater has passwords today. A slot with no ACL
     refuses every login, which is the honest answer: there is nothing on it to
     authorise. */
  bool slotLogin(int idx, const char* password, uint8_t& perms) override {
    if (!slotLive(idx)) return false;
    HydraSlot* s = hydra.slot(idx);
    if (s == NULL || s->type() != SLOT_REPEATER) return false;
    NodePrefs* p = prefs0();
    return companion::loginPermissions(password, p->password, p->guest_password, &perms);
  }

  int slotRequest(int idx, uint8_t perms, const uint8_t* req, size_t req_len,
                  uint8_t* dest, size_t max_len) override {
    if (!slotLive(idx) || req_len < 1) return -1;
    HydraSlot* s = hydra.slot(idx);
    if (s == NULL || s->type() != SLOT_REPEATER) return -1;

    if (req[0] == COMP_REQ_GET_STATUS) {
      companion::Status st;
      fillRepeaterStatus(st);
      return (int)companion::encodeStatusBlob(dest, max_len, st);
    }
    if (req[0] == COMP_REQ_KEEP_ALIVE) {
      // The session lives as long as the BLE link, so there is nothing to keep
      // alive. An empty answer still tells the app that the node is there.
      return 0;
    }
    // The ACL, the neighbours and the telemetry are answered by private code in
    // examples/simple_repeater/MyMesh.cpp. Reaching them needs a seam upstream
    // that does not exist. Until it does, an operator reads them over the CLI on
    // this same link.
    return -1;
  }

  // ---------------------------------------------------------------- the CLI

  void handleCliCommand(uint32_t sender_timestamp, char* command,
                        char* reply, size_t reply_sz) override {
    hydra.handleCommand(sender_timestamp, command, reply, reply_sz);
  }

private:
  /* The counters of a shared radio belong to the node, and the counters of a
     route belong to the identity. So the packet totals, the airtime and the
     receive errors come from the arbiter and the driver, and the flood and
     direct counts come from the mesh of slot 0.

     Two fields are zero, and neither is reachable without a change to upstream:
     the outbound queue length lives behind Dispatcher::_mgr, and the error
     flags live in Dispatcher::_err_flags. Both are protected. */
  void fillRepeaterStatus(companion::Status& st) {
    memset(&st, 0, sizeof(st));
    MyMesh& m = hydra.repeater().mesh();
    SharedRadioCore& core = hydra.radio();

    st.batt_milli_volts = board.getBattMilliVolts();
    st.curr_tx_queue_len = 0;
    st.noise_floor = (int16_t)radio_driver.getNoiseFloor();
    st.last_rssi = (int16_t)radio_driver.getLastRSSI();
    st.n_packets_recv = radio_driver.getPacketsRecv();
    st.n_packets_sent = radio_driver.getPacketsSent();
    st.total_air_time_secs = core.txAirtimeMs() / 1000;
    st.total_up_time_secs = millis() / 1000;
    st.n_sent_flood = m.getNumSentFlood();
    st.n_sent_direct = m.getNumSentDirect();
    st.n_recv_flood = m.getNumRecvFlood();
    st.n_recv_direct = m.getNumRecvDirect();
    st.err_events = 0;
    st.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    st.n_direct_dups = (uint16_t)hydra.repeater().tables().getNumDirectDups();
    st.n_flood_dups = (uint16_t)hydra.repeater().tables().getNumFloodDups();
    st.total_rx_air_time_secs = core.rxAirtimeMs() / 1000;
    st.n_recv_errors = radio_driver.getPacketsRecvErrors();
  }
};
