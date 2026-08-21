#include "HydraNode.h"
#include "PeerReport.h"

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

HydraNode hydra;

// The arbiter applies the TX power of a port just before that port transmits.
// Identities can thus run at different powers on one PA. RadioLibWrapper has
// setTxPower(), but it does not implement the interface. So this class adapts
// it here, and no change is necessary in src/.
class RadioTxPower : public TxPowerControl {
public:
  void applyTxPower(int8_t dbm) override { radio_driver.setTxPower(dbm); }
};
static RadioTxPower s_tx_power;

// This function repairs a stuck transceiver. It restores every parameter that
// MyMesh::begin() applies. It does not restore only the parameters that
// radio_init() leaves at the driver defaults. If the radio came back on the
// default frequency of the driver, it would be off-band with no warning. That
// is worse than the fault that the function repairs. The RX-silence recovery of
// the arbiter and the node LoRa watchdog below both use this function.
static void hydra_radio_reinit() {
  Serial.println("hydra: radio silent, re-initialising");
  if (!radio_init()) {
    Serial.println("hydra: radio re-init FAILED");
    return;
  }
  radio_driver.begin();   // this attaches the DIO1 ISR again, which radio_init() removed
  NodePrefs* p = hydra.repeater().prefs();
  radio_driver.setParams(p->freq, p->bw, p->sf, p->cr);
  radio_driver.setTxPower(p->tx_power_dbm);
  radio_driver.setRxBoostedGainMode(p->rx_boosted_gain);
  board.setLoRaFemLnaEnabled(p->radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(p->radio_fem_txgain);
}

#ifdef LORA_WATCHDOG_MS
/* There is one watchdog for the board, not one for each identity. Detection
   still works when the slots share the radio. RX goes to every port, so a dead
   radio is silent for every slot. But the response does not work when the slots
   share the radio. N slots would each send a probe and each decide to reboot.
   The airtime comes from the arbiter. The arbiter is the only part here that
   sees all the traffic of the real radio. The probe uses the identity of
   slot 0, because one fault needs one advert and not N adverts. */
static uint32_t lora_wd_airtime(void*) { return hydra.radio().airtimeMs(); }
static void lora_wd_probe(void*) {
  hydra.repeater().mesh().sendSelfAdvertisement(500, false);   // zero-hop, no flood
}
static void lora_wd_reinit(void*) { hydra_radio_reinit(); }
static void lora_wd_reboot(void*) {
  hydra.flushPendingWrites();   // every slot. The code also delays the room ACL writes.
  board.reboot();
}
#endif

static uint32_t rx_err_count() { return radio_driver.getPacketsRecvErrors(); }

// This function reports which of the three conditions in
// RadioLibWrapper::isReceiving() delayed us. isReceivingPacket() is protected.
// So the code calculates the RSSI test again from public state. That test is
// the test of the driver itself, word for word. The code then separates the
// other paths by whether CAD is enabled. When CAD is off, the preamble and
// header IRQ is the only path left, and the answer is exact.
static int hydra_channel_busy_probe() {
  int thresh = hydra.radio().interferenceThreshold();
  if (thresh != 0 && radio_driver.getCurrentRSSI() > radio_driver.getNoiseFloor() + thresh) {
    return TXWAIT_RSSI;
  }
  if (!hydra.radio().cadEnabled()) return TXWAIT_RX_PACKET;
  // The code reaches this line only when CAD is on. So the extra scan costs
  // nothing new. The node already paid for that scan at every send attempt.
  return radio_driver.isChannelActive() ? TXWAIT_CAD : TXWAIT_RX_PACKET;
}

HydraNode::HydraNode() : _core(radio_driver), _fs(NULL) {
  memset(_cfg, 0, sizeof(_cfg));
  _slots[0] = &_slot0;
  _cfg[0].type = SLOT_REPEATER;   // decision D: slot 0 is always the repeater
#if HYDRA_NUM_CHAT_SLOTS > 0
  for (int i = 0; i < HYDRA_NUM_CHAT_SLOTS; i++) {
    _slots[i + 1] = &_chat[i];
    // Decision H. A new node has slot 0 only. Every other slot is optional. A
    // slot has no name until an operator gives it one.
    _cfg[i + 1].type = SLOT_OFF;
    _cfg[i + 1].advert_mins = HYDRA_CHAT_ADVERT_MINS;
  }
#endif
}

const char* HydraNode::typeName(SlotType t) {
  switch (t) {
    case SLOT_REPEATER: return "repeater";
    case SLOT_CHAT:     return "chat";
    case SLOT_ROOM:     return "room";
    default:            return "off";
  }
}

void HydraNode::slotIdName(int idx, char* dest, size_t sz) {
  // Slot 0 keeps the standard "_main" name. A board that you reflash from
  // simple_repeater to hydra thus comes back as the same node to the mesh.
  //
  // Every other slot is "_slotN". That name comes from the INDEX and from
  // nothing else. It does not come from the type. It does not come from the
  // display name. This is the path that holds a keypair. If the path moves, the
  // node loses the identity (decision 7).
  if (idx == 0) StrHelper::strncpy(dest, "_main", sz);
  else snprintf(dest, sz, "_slot%d", idx);
}

void HydraNode::begin(FILESYSTEM* fs) {
  _fs = fs;
  loadSlotConfig();

  // The node registers the port of every slot before it pumps anything. It does
  // this for an active slot and for an inactive slot. Thus port index == slot
  // index, and the packet trace stays easy to read.
  for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
    int idx = _core.addPort(_slots[i]->port(), _cfg[i].type != SLOT_OFF);
    _core.setPortName(idx, i == 0 ? "repeater" : typeName(_cfg[i].type));
  }
  _core.setTxPowerControl(&s_tx_power);
  _core.setRadioReinit(hydra_radio_reinit);
  _core.setRxErrorCounter(rx_err_count);
  _core.setChannelBusyProbe(hydra_channel_busy_probe);
  // The rest of the mesh depends on the routes. It does not depend on the chat
  // traffic. The code gives each port a rank. It does not make slot 0 a special
  // case. You can thus give a room server the same rank, and the arbiter does
  // not have to know what a slot is.
  _core.setPortPriority(0, 0);
  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) _core.setPortPriority(i, 1);
  // Identities on one antenna cannot hear each other, because the radio is half
  // duplex. So slot 0 could never repeat for slot 1. Loopback makes each
  // transmit look like a receive to the other ports. That is what a second
  // physical node in the same room would hear.
  _core.setLoopback(true);
  _core.observer().setClock(&rtc_clock);   // the advert clocks of peers then have a meaning

  // The code seeds the RNG once, for the whole node. StdRNG is a front end for
  // the global Arduino PRNG. A seed for each slot would seed the same generator
  // again. And each call asks the shared transceiver for entropy.
  StdRNG seeder;
  seeder.begin(radio_driver.getRngSeed());

  startSlot(0);   // the repeater is always on, whatever the config says
  NodePrefs* p = _slot0.prefs();
  // The duty cycle belongs to the antenna. So the whole node takes from one
  // pool. The airtime_factor of slot 0 sets that pool. The chat slots still
  // have their own Dispatcher budgets, but those budgets no longer limit the
  // node.
  _core.setDutyCycle(p->airtime_factor);
#ifdef LORA_WATCHDOG_MS
  _lora_wd.begin(LORA_WATCHDOG_MS, this, lora_wd_airtime, lora_wd_probe,
                 lora_wd_reinit, lora_wd_reboot);
#endif
  _core.setCodingRate(p->cr);
  _slot0.port().setPortTxPower(p->tx_power_dbm);
  _core.setPortIdentity(0, _slot0.identity().pub_key);

  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) {
    if (_cfg[i].type == SLOT_OFF) continue;
    SlotEnableResult r = startSlot(i);
    if (r == SLOT_ENABLE_OK) {
      _slots[i]->port().setPortTxPower(p->tx_power_dbm);
      _core.setPortIdentity(i, _slots[i]->identity().pub_key);
    } else {
      // A slot that the node refuses at boot stays off for this boot. It keeps
      // its config. Thus `slots` still shows the reason, and the code rewrites
      // nothing without a warning.
      _core.setPortActive(i, false);
      Serial.printf("hydra: slot %d not started - %s\n", i, slotEnableError(r));
    }
  }
}

SlotEnableResult HydraNode::startSlot(int idx) {
  if (idx == 0) {
    char id_name[16];
    slotIdName(0, id_name, sizeof(id_name));
    IdentityStore store(*_fs, "");
    if (!_slot0.begin(_fs, store, id_name, NULL, SLOT_REPEATER)) return SLOT_ENABLE_FAILED;
    _core.setPortActive(0, true);
    Serial.print("hydra: slot 0 (repeater) id ");
    mesh::Utils::printHex(Serial, _slot0.identity().pub_key, PUB_KEY_SIZE);
    Serial.println();
    return SLOT_ENABLE_OK;
  }

  SlotEnableResult chk = slotEnableCheck(idx, HYDRA_NUM_SLOTS, _cfg[idx].type, _cfg[idx].name);
  if (chk != SLOT_ENABLE_OK) return chk;

  /* This is the reserve floor of decision G. Pin the floor first. Then let the
     slot allocate what is left. A slot that starts has thus taken none of the
     reserve. The node refuses a slot that cannot do this. It refuses that slot
     NOW, and allocNew() does not return NULL at 3am. Nothing here measures the
     free heap. The free heap at boot is not the free heap at peak load, and
     this node must run with no operator present. */
  RamFloor floor(HYDRA_RAM_RESERVE);
  if (!floor.held()) return SLOT_ENABLE_NO_RAM;

  char id_name[16];
  slotIdName(idx, id_name, sizeof(id_name));
  IdentityStore store(*_fs, "");
  if (!_slots[idx]->begin(_fs, store, id_name, _cfg[idx].name, _cfg[idx].type)) {
    return SLOT_ENABLE_NO_RAM;   // begin() fails only when the heap refused
  }
  floor.release();
  _core.setPortActive(idx, true);
  _core.setPortName(idx, typeName(_cfg[idx].type));

  Serial.printf("hydra: slot %d (%s) \"%s\" id ", idx, typeName(_slots[idx]->type()),
                _cfg[idx].name);
  mesh::Utils::printHex(Serial, _slots[idx]->identity().pub_key, PUB_KEY_SIZE);
  Serial.println();
  return SLOT_ENABLE_OK;
}

void HydraNode::stopSlot(int idx) {
  if (idx <= 0 || idx >= HYDRA_NUM_SLOTS) return;   // you cannot disable slot 0
  // The identity object stays in memory. The code silences only its port. The
  // slot then stops all receive and all transmit at once. The slot can come
  // back with no call to the heap. Its keypair, its ACL and its contacts stay
  // as they are on the disk.
  _slots[idx]->flushPendingWrites();
  _core.setPortActive(idx, false);
}

bool HydraNode::hasPendingWork() const {
  // This checks EVERY active slot, not only slot 0. The powersave gate puts the
  // board to sleep. The code must not read the queue of a slot only when slot 0
  // is busy. That slot would then stop with no warning. It would stay stopped
  // for as long as slot 0 stayed quiet.
  for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
    if (_core.portActive(i) && _slots[i]->hasPendingWork()) return true;
  }
  return _core.txInFlight();
}

void HydraNode::flushPendingWrites() {
  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) _slots[i]->flushPendingWrites();
  _slot0.mesh().flushPendingWrites();
}

void HydraNode::loop() {
  // THE ORDER IS IMPORTANT (see SharedRadio.h). Every identity takes the
  // current frame before the arbiter gets the next one.
  for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
    if (_core.portActive(i)) _slots[i]->loop();
  }
  _core.pump();
#ifdef LORA_WATCHDOG_MS
  _lora_wd.loop();   // this paces itself. See LoraWatchdog::CHECK_EVERY_MS.
#endif
}

// ---------------------------------------------------------------- slot config

// This is a binary record of a fixed size, and not JSON. That is on purpose.
// The code reads it once at boot, before any identity exists. And the CommonCLI
// of slot 0 already owns prefs.json.
//
// SAFE MODE. Every failure path here gives "a plain repeater, with the other
// slots off" (decision D). A file that is not there makes the function return
// at once. A bad magic number keeps the defaults. A short read breaks the loop
// and keeps what came before it. A type that this build does not implement
// becomes SLOT_OFF, and the code does not give it a new meaning. A stored name
// that fails the check of the CLI also disables the slot. The rule that no
// identity has an empty name thus stays true after a reboot and after a corrupt
// file.
void HydraNode::loadSlotConfig() {
  if (!_fs->exists(HYDRA_SLOT_CFG_FILE)) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(HYDRA_SLOT_CFG_FILE, "r");
#else
  File f = _fs->open(HYDRA_SLOT_CFG_FILE);
#endif
  if (!f) return;
  uint8_t hdr[2];
  if (f.read(hdr, 2) != 2 || hdr[0] != 'H') { f.close(); return; }

  if (hdr[1] == 1) {
    /* v1 had no names. It had only one type byte for each slot. An enabled v1
       slot is already on the mesh under the name that ChatSlot made for it. So
       the code moves it to exactly that string. It does not disable an identity
       that works. */
    for (int i = 1; i < HYDRA_NUM_SLOTS; i++) {
      uint8_t t = SLOT_OFF;
      if (f.read(&t, 1) != 1) break;
      if (t != SLOT_CHAT) { _cfg[i].type = SLOT_OFF; continue; }
      _cfg[i].type = SLOT_CHAT;
      snprintf(_cfg[i].name, sizeof(_cfg[i].name), "%s slot%d", HYDRA_NAME_PREFIX, i);
    }
    f.close();
    saveSlotConfig();   // rewrite as v2, so the migration happens once
    return;
  }
  if (hdr[1] != 2) { f.close(); return; }

  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) {
    uint8_t rec[4];
    char name[SLOT_NAME_MAX];
    // A slot count that got smaller between two builds leaves old bytes. The
    // code must not apply a part of a record at the tail.
    if (f.read(rec, 4) != 4) break;
    if (f.read((uint8_t*)name, SLOT_NAME_MAX) != SLOT_NAME_MAX) break;
    name[SLOT_NAME_MAX - 1] = 0;

    StrHelper::strncpy(_cfg[i].name, name, sizeof(_cfg[i].name));
    _cfg[i].advert_mins = rec[1];
    _cfg[i].flood = rec[2] != 0;
    _cfg[i].type = slotTypeFromRecord(rec[0], _cfg[i].name);
  }
  f.close();
}

void HydraNode::saveSlotConfig() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(HYDRA_SLOT_CFG_FILE);
  File f = _fs->open(HYDRA_SLOT_CFG_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File f = _fs->open(HYDRA_SLOT_CFG_FILE, "w");
#else
  File f = _fs->open(HYDRA_SLOT_CFG_FILE, "w", true);
#endif
  if (!f) return;
  uint8_t hdr[2] = { 'H', 2 };
  f.write(hdr, 2);
  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) {
    uint8_t rec[4] = { (uint8_t)_cfg[i].type, _cfg[i].advert_mins,
                       (uint8_t)(_cfg[i].flood ? 1 : 0), 0 };
    char name[SLOT_NAME_MAX];
    memset(name, 0, sizeof(name));
    StrHelper::strncpy(name, _cfg[i].name, sizeof(name));
    f.write(rec, 4);
    f.write((uint8_t*)name, SLOT_NAME_MAX);
  }
  f.close();
}

// ----------------------------------------------------------------------- CLI

void HydraNode::formatSlotTable(char* reply, size_t reply_sz) {
  int n = snprintf(reply, reply_sz, "slots:");
  for (int i = 0; i < HYDRA_NUM_SLOTS && n > 0 && (size_t)n < reply_sz; i++) {
    bool live = _core.portActive(i);
    char key[9] = "-";
    if (live) mesh::Utils::toHex(key, _slots[i]->identity().pub_key, 4);
    const char* nm = (i == 0) ? _slot0.name() : _cfg[i].name;
    int w = snprintf(reply + n, reply_sz - n, " [%d]%s%s \"%s\" %s rx=%u tx=%u;", i,
                     typeName(_cfg[i].type), live ? "" : "(down)",
                     nm[0] ? nm : "(unnamed)", key,
                     (unsigned)_core.portRxCount(i), (unsigned)_core.portTxCount(i));
    if (w < 0) break;
    n += w;
  }
  // Decision G asks for the headroom here. This is a probe. It is not a
  // measurement of the free heap, and nothing sets its own size from it. It is
  // the largest single block that the allocator still gives out now.
  if (n > 0 && (size_t)n < reply_sz) {
    size_t largest = probeLargestBlock(64 * 1024);
    snprintf(reply + n, reply_sz - n, " reserve=%uB largest-block=%uB",
             (unsigned)HYDRA_RAM_RESERVE, (unsigned)largest);
  }
}

void HydraNode::reportPeers(char* reply, size_t reply_sz) {
  const MeshObserver& obs = _core.observer();
  char line[160];
  formatPeerSummary(line, sizeof(line), obs);
  Serial.println(line);
  Serial.println(peerColumnHeader());
  uint32_t now = millis();
  for (int i = 0; i < obs.numPeers(); i++) {
    const MeshObserver::PeerEntry* e = obs.peer(i);
    if (e == NULL) continue;
    formatPeerRow(line, sizeof(line), *e, now);
    Serial.println(line);
  }
  formatPeerSummary(reply, reply_sz, obs);
}

void HydraNode::handleCommand(uint32_t sender_timestamp, char* command,
                              char* reply, size_t reply_sz) {
  reply[0] = 0;

  if (strcmp(command, "slots") == 0) {
    formatSlotTable(reply, reply_sz);
    return;
  }
  if (strcmp(command, "peers") == 0) {
    /* Decision E. This is a map of who can hear whom in the neighbourhood. It
       comes from third parties who never agreed to be in it. So it does not go
       out over the air. Upstream applies the same rule to `get acl` and to
       `get prv.key`. Physical access to the console is the authentication. */
    if (sender_timestamp != 0) {
      StrHelper::strncpy(reply, "ERR: peers is console-only - it describes third parties", reply_sz);
      return;
    }
    reportPeers(reply, reply_sz);
    return;
  }
  if (strcmp(command, "stats-shared") == 0) {   // the arbiter view, for the whole node
    snprintf(reply, reply_sz,
             "rx=%u tx=%u contend=%u stuck=%u refused=%u dropped=%u peers=%d idle=%us "
             "duty=%u/%us used=%us",
             (unsigned)_core.rxTotal(), (unsigned)_core.txTotal(),
             (unsigned)_core.txContention(), (unsigned)_core.txStuck(),
             (unsigned)_core.txRefused(), (unsigned)_core.rxDropped(),
             _core.numPeers(), (unsigned)(_core.msSinceLastRx() / 1000),
             (unsigned)(_core.txBudgetMs() / 1000), (unsigned)(_core.txBudgetMaxMs() / 1000),
             (unsigned)(_core.txChargedMs() / 1000));
    return;
  }
  if (strcmp(command, "trace") == 0) {          // the packet trace, newest entry last
#if PKT_TRACE_ENTRIES
    Serial.printf("trace: %d entries x %d raw bytes, air rx=%us tx=%us\n",
                  SharedRadioCore::PKT_LOG_SIZE, SharedRadioCore::PKT_LOG_RAW_CAP,
                  (unsigned)(_core.rxAirtimeMs() / 1000), (unsigned)(_core.txAirtimeMs() / 1000));
    static const char* kFlag[] = { "ok", "rxerr", "txfail", "txbusy" };
    PktLogEntry e[4];
    uint32_t seq = _core.pktLogSeq();
    uint32_t after = seq > (uint32_t)SharedRadioCore::PKT_LOG_SIZE
                     ? seq - SharedRadioCore::PKT_LOG_SIZE : 0;
    int total = 0;
    for (int n; (n = _core.pktLogCopy(e, 4, after)) > 0; ) {
      for (int i = 0; i < n; i++) {
        const PktLogEntry& x = e[i];
        Serial.printf("%8lu %s %-6s hdr=%02X len=%3u air=%ums cr=4/%u snr=%d rssi=%d "
                      "hash=%02x%02x%02x%02x\n",
                      (unsigned long)x.t_ms,
                      x.dir < 0 ? "rx" : _core.portName(x.dir),
                      x.flag < 4 ? kFlag[x.flag] : "?",
                      x.hdr, (unsigned)x.len, (unsigned)x.air_ms, (unsigned)x.cr,
                      x.snr4 / 4, x.rssi,
                      x.hash[0], x.hash[1], x.hash[2], x.hash[3]);
        after = x.seq;
      }
      total += n;
    }
    snprintf(reply, reply_sz, "OK - %d entries", total);
#else
    // The build removes this. It is not broken. PKT_TRACE_ENTRIES is 0 in a
    // shipped build, because the ring is 10.9 KB of RAM at the full depth.
    StrHelper::strncpy(reply, "trace disabled - build with -D PKT_TRACE_ENTRIES=N", reply_sz);
#endif
    return;
  }
  if (strcmp(command, "stats-txwait") == 0) {   // why we could not transmit
    snprintf(reply, reply_sz,
             "budget=%u sibling=%u prio=%u rxpkt=%u rssi=%u cad=%u radio=%u forced=%u",
             (unsigned)_core.txWaits(TXWAIT_BUDGET), (unsigned)_core.txWaits(TXWAIT_SIBLING),
             (unsigned)_core.txWaits(TXWAIT_PRIORITY), (unsigned)_core.txWaits(TXWAIT_RX_PACKET),
             (unsigned)_core.txWaits(TXWAIT_RSSI), (unsigned)_core.txWaits(TXWAIT_CAD),
             (unsigned)_core.txWaits(TXWAIT_RADIO), (unsigned)_core.txWaits(TXWAIT_FORCED));
    return;
  }

  if (strncmp(command, "slot ", 5) == 0) {
    char* arg = command + 5;
    int idx = atoi(arg);
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    if (idx < 0 || idx >= HYDRA_NUM_SLOTS) {
      snprintf(reply, reply_sz, "ERR: slot 0..%d", HYDRA_NUM_SLOTS - 1);
      return;
    }
    handleSlotCommand(idx, sender_timestamp, arg, reply, reply_sz);
    return;
  }

  // A command with no slot prefix goes to slot 0. A hydra node still answers
  // the whole repeater CLI. That fall-through IS the node namespace. The radio,
  // the watchdogs, the LED and the board all come from the prefs of slot 0,
  // because there is one of each.
  _slot0.handleCommand(sender_timestamp, command, reply, reply_sz);
}

// -------------------------------------------------------------- `slot N ...`

void HydraNode::handleSlotCommand(int idx, uint32_t sender_timestamp, char* arg,
                                  char* reply, size_t reply_sz) {
  // Slot 0 is the repeater. Its CLI is the standard one, with or without a
  // slot prefix.
  if (idx == 0) {
    if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0 ||
        strcmp(arg, "chat") == 0 || strcmp(arg, "room") == 0) {
      StrHelper::strncpy(reply, slotEnableError(SLOT_ENABLE_SLOT0), reply_sz);
      return;
    }
    _slot0.handleCommand(sender_timestamp, arg, reply, reply_sz);
    return;
  }

  if (strncmp(arg, "set ", 4) == 0) { handleSlotSet(idx, sender_timestamp, arg + 4, reply, reply_sz); return; }
  if (strncmp(arg, "get ", 4) == 0) { handleSlotGet(idx, sender_timestamp, arg + 4, reply, reply_sz); return; }

  SlotType want = SLOT_OFF;
  if (strcmp(arg, "on") == 0)        want = (_cfg[idx].type != SLOT_OFF) ? _cfg[idx].type : SLOT_CHAT;
  else if (strcmp(arg, "chat") == 0) want = SLOT_CHAT;
  else if (strcmp(arg, "room") == 0) want = SLOT_ROOM;

  if (want != SLOT_OFF) {
    /* A slot that already ran in this boot has a mesh object for the type that
       it started as. And `slot N off` only silences the port. type() is
       SLOT_OFF until begin() runs. So this test also catches a slot that
       stopped. That slot is the one that would otherwise come back as the wrong
       type. */
    if (_slots[idx]->type() != SLOT_OFF && _slots[idx]->type() != want) {
      snprintf(reply, reply_sz, "ERR: slot %d already ran as %s this boot - reboot to retype",
               idx, typeName(_slots[idx]->type()));
      return;
    }
    /* Decision H. The name gate runs BEFORE the code stores or starts anything.
       A refused enable thus leaves no trace. And an identity never adverts with
       no name. */
    SlotEnableResult chk = slotEnableCheck(idx, HYDRA_NUM_SLOTS, want, _cfg[idx].name);
    if (chk != SLOT_ENABLE_OK) { StrHelper::strncpy(reply, slotEnableError(chk), reply_sz); return; }

    SlotType prev = _cfg[idx].type;
    _cfg[idx].type = want;
    SlotEnableResult r = startSlot(idx);
    if (r != SLOT_ENABLE_OK) {
      _cfg[idx].type = prev;   // nothing saved, nothing started
      StrHelper::strncpy(reply, slotEnableError(r), reply_sz);
      return;
    }
    saveSlotConfig();
    _core.setPortIdentity(idx, _slots[idx]->identity().pub_key);
    _slots[idx]->port().setPortTxPower(_slot0.prefs()->tx_power_dbm);
    snprintf(reply, reply_sz, "OK - slot %d %s \"%s\"", idx, typeName(want), _cfg[idx].name);
    return;
  }

  if (strcmp(arg, "off") == 0) {
    _cfg[idx].type = SLOT_OFF;
    saveSlotConfig();
    stopSlot(idx);
    snprintf(reply, reply_sz, "OK - slot %d off", idx);
    return;
  }

  if (slotVerbIsNodeLevel(arg)) { StrHelper::strncpy(reply, slotNodeLevelError(), reply_sz); return; }
  _slots[idx]->handleCommand(sender_timestamp, arg, reply, reply_sz);
}

void HydraNode::handleSlotSet(int idx, uint32_t sender_timestamp, char* arg,
                              char* reply, size_t reply_sz) {
  /* Decision 8. The node refuses a slot that asks for a radio parameter. There
     is one transceiver. If slot 3 retuned it, every other identity would go off
     the air with it. */
  if (slotVerbIsNodeLevel(arg)) { StrHelper::strncpy(reply, slotNodeLevelError(), reply_sz); return; }

  if (strncmp(arg, "name ", 5) == 0) {
    const char* v = arg + 5;
    if (!slotNameValid(v)) {
      snprintf(reply, reply_sz, "ERR: bad name (1-%d chars, no [ ] \\ : , ? *)", SLOT_NAME_MAX - 1);
      return;
    }
    StrHelper::strncpy(_cfg[idx].name, v, sizeof(_cfg[idx].name));
    saveSlotConfig();
    _slots[idx]->setName(_cfg[idx].name);   // live: the next advert carries the name
    snprintf(reply, reply_sz, "OK - slot %d is \"%s\"", idx, _cfg[idx].name);
    return;
  }
  if (strncmp(arg, "prv.key ", 8) == 0) {
#if ENABLE_PRIVATE_KEY_IMPORT
    setSlotPrivateKey(idx, arg + 8, reply, reply_sz);
#else
    // Upstream puts the key import behind this flag. Its comment reads "comment
    // these out for more secure firmware". When the flag is off, the command
    // does not exist.
    StrHelper::strncpy(reply, "ERR: unknown setting", reply_sz);
#endif
    return;
  }
  if (strncmp(arg, "advert.interval ", 16) == 0) {
    // These are the same limits as the node-level advert.interval in CommonCLI.
    // A value of 0 means "never". Every other value must be one hour or more.
    // There is one antenna and there are N identities. Without the lower limit,
    // the node would send N times as many adverts.
    int mins = atoi(arg + 16);
    if (mins < 0 || (mins > 0 && mins < 60) || mins > 240) {
      StrHelper::strncpy(reply, "ERR: interval is 0 (never) or 60-240 minutes", reply_sz);
      return;
    }
    _cfg[idx].advert_mins = (uint8_t)mins;
    saveSlotConfig();
#if HYDRA_NUM_CHAT_SLOTS > 0
    _chat[idx - 1].mesh().setAdvertMins((uint8_t)mins);
#endif
    snprintf(reply, reply_sz, "OK - slot %d adverts every %d min", idx, mins);
    return;
  }
  if (strncmp(arg, "flood.advert ", 13) == 0) {
    _cfg[idx].flood = strncmp(arg + 13, "on", 2) == 0;
    saveSlotConfig();
#if HYDRA_NUM_CHAT_SLOTS > 0
    _chat[idx - 1].mesh().setFloodAdvert(_cfg[idx].flood);
#endif
    snprintf(reply, reply_sz, "OK - slot %d flood advert %s", idx, _cfg[idx].flood ? "on" : "off");
    return;
  }
  StrHelper::strncpy(reply, "ERR: unknown setting", reply_sz);
}

void HydraNode::handleSlotGet(int idx, uint32_t sender_timestamp, char* arg,
                              char* reply, size_t reply_sz) {
  if (slotVerbIsNodeLevel(arg)) { StrHelper::strncpy(reply, slotNodeLevelError(), reply_sz); return; }

  if (strcmp(arg, "name") == 0) {
    snprintf(reply, reply_sz, "> %s", _cfg[idx].name[0] ? _cfg[idx].name : "(unset)");
  } else if (strcmp(arg, "type") == 0) {
    snprintf(reply, reply_sz, "> %s", typeName(_cfg[idx].type));
  } else if (strcmp(arg, "advert.interval") == 0) {
    snprintf(reply, reply_sz, "> %d", (int)_cfg[idx].advert_mins);
  } else if (strcmp(arg, "flood.advert") == 0) {
    snprintf(reply, reply_sz, "> %s", _cfg[idx].flood ? "on" : "off");
  } else if (strcmp(arg, "public.key") == 0) {
    if (!_core.portActive(idx)) { StrHelper::strncpy(reply, "ERR: slot not running", reply_sz); return; }
    reply[0] = '>'; reply[1] = ' ';
    mesh::Utils::toHex(&reply[2], _slots[idx]->identity().pub_key, PUB_KEY_SIZE);
#if ENABLE_PRIVATE_KEY_EXPORT
  } else if (sender_timestamp == 0 && strcmp(arg, "prv.key") == 0) {
    // This is for the console only, like `get prv.key` upstream. The key IS the
    // identity. The command is useful on a slot whose key this node made.
    // Otherwise the only copy is on a filesystem that nobody has backed up.
    if (!_core.portActive(idx)) { StrHelper::strncpy(reply, "ERR: slot not running", reply_sz); return; }
    uint8_t prv[PRV_KEY_SIZE];
    mesh::LocalIdentity id = _slots[idx]->identity();
    int len = id.writeTo(prv, PRV_KEY_SIZE);
    reply[0] = '>'; reply[1] = ' ';
    mesh::Utils::toHex(&reply[2], prv, len);
#endif
  } else {
    _slots[idx]->handleCommand(sender_timestamp, arg, reply, reply_sz);
  }
}

#if ENABLE_PRIVATE_KEY_IMPORT
/* How to move an existing identity onto a slot.

   THIS WRITES A KEYPAIR TO FLASH. This project has lost identities to that
   operation before (see slot_types.h on origin/time-converge). There are three
   rules. All three are here on purpose:

   1. The storage name comes from slotIdName(), that is, from the slot INDEX.
      begin() loads the identity with the same call. There is no second
      convention. No path depends on the type of the slot or on its display
      name.
   2. The slot must be OFF. An identity that runs has contacts, an ACL and a
      mesh object that all use the old key. A change below them would leave a
      node that is half one identity and half another. OFF means that the file
      is the only state, and begin() reads that file again.
   3. Write the key. Then READ IT BACK and compare it before you report success.
      If you do not, a short or failed LittleFS write reports OK. You then find
      the fault at the next boot, and the old key is already gone. */
bool HydraNode::setSlotPrivateKey(int idx, const char* hex, char* reply, size_t reply_sz) {
  /* Rule 2. type() is SLOT_OFF only while begin() has never run. So this test
     also refuses a slot that started and then went off. Its mesh object, its
     contacts and its ACL in RAM still use the old identity. And `slot N on`
     would not read the file again. */
  if (_slots[idx]->type() != SLOT_OFF) {
    StrHelper::strncpy(reply, "ERR: slot has run this boot - reboot with it off, then set the key", reply_sz);
    return false;
  }
  uint8_t prv[PRV_KEY_SIZE];
  if (!mesh::Utils::fromHex(prv, PRV_KEY_SIZE, hex) ||
      !mesh::LocalIdentity::validatePrivateKey(prv)) {
    StrHelper::strncpy(reply, "ERR: bad key", reply_sz);
    return false;
  }
  mesh::LocalIdentity new_id;
  new_id.readFrom(prv, PRV_KEY_SIZE);

  char id_name[16];
  slotIdName(idx, id_name, sizeof(id_name));   // rule 1: the index, and nothing else
  IdentityStore store(*_fs, "");
  if (!store.save(id_name, new_id)) {
    StrHelper::strncpy(reply, "ERR: could not write identity - key NOT changed", reply_sz);
    return false;
  }
  mesh::LocalIdentity check;                   // rule 3: read it back
  if (!store.load(id_name, check) ||
      memcmp(check.pub_key, new_id.pub_key, PUB_KEY_SIZE) != 0) {
    StrHelper::strncpy(reply, "ERR: identity did not read back - DO NOT enable this slot", reply_sz);
    return false;
  }
  StrHelper::strncpy(reply, "OK - stored, `slot N on` to use it. New pubkey: ", reply_sz);
  size_t at = strlen(reply);
  if (at + PUB_KEY_SIZE * 2 + 1 < reply_sz) {
    mesh::Utils::toHex(&reply[at], new_id.pub_key, PUB_KEY_SIZE);
  }
  return true;
}
#endif
