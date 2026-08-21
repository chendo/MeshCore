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

// The arbiter applies each port's TX power just before that port transmits, so
// identities can run at different powers on one PA. RadioLibWrapper has
// setTxPower() but does not implement the interface, so adapt it here rather
// than reach into src/.
class RadioTxPower : public TxPowerControl {
public:
  void applyTxPower(int8_t dbm) override { radio_driver.setTxPower(dbm); }
};
static RadioTxPower s_tx_power;

// Putting a wedged transceiver back together. Restores every parameter
// MyMesh::begin() applies, not just the ones radio_init() leaves at driver
// defaults -- coming back on the driver's default frequency is silently
// off-band, which is worse than the fault being repaired. Shared by the
// arbiter's RX-silence recovery and by the node LoRa watchdog below.
static void hydra_radio_reinit() {
  Serial.println("hydra: radio silent, re-initialising");
  if (!radio_init()) {
    Serial.println("hydra: radio re-init FAILED");
    return;
  }
  radio_driver.begin();   // re-attaches the DIO1 ISR that radio_init() dropped
  NodePrefs* p = hydra.repeater().prefs();
  radio_driver.setParams(p->freq, p->bw, p->sf, p->cr);
  radio_driver.setTxPower(p->tx_power_dbm);
  radio_driver.setRxBoostedGainMode(p->rx_boosted_gain);
  board.setLoRaFemLnaEnabled(p->radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(p->radio_fem_txgain);
}

#ifdef LORA_WATCHDOG_MS
/* One watchdog for the board, not one per identity. Detection survives sharing
   -- RX fans out, so a dead radio goes silent for every slot -- but the
   response does not: N slots would each probe and each decide to reboot. The
   airtime comes from the arbiter, which is the only thing here that sees the
   real radio's whole traffic; the probe is slot 0's identity, since a fault
   needs one advert, not N. */
static uint32_t lora_wd_airtime(void*) { return hydra.radio().airtimeMs(); }
static void lora_wd_probe(void*) {
  hydra.repeater().mesh().sendSelfAdvertisement(500, false);   // zero-hop, no flood
}
static void lora_wd_reinit(void*) { hydra_radio_reinit(); }
static void lora_wd_reboot(void*) {
  hydra.flushPendingWrites();   // every slot: room ACLs are lazy-written too
  board.reboot();
}
#endif

static uint32_t rx_err_count() { return radio_driver.getPacketsRecvErrors(); }

// Which of the three conditions behind RadioLibWrapper::isReceiving() actually
// deferred us. isReceivingPacket() is protected, so the RSSI test is recomputed
// from public state (it is the driver's own test, verbatim) and the remaining
// paths are separated by whether CAD is even enabled — when it is not, the
// preamble/header IRQ is the only one left and the answer is exact.
static int hydra_channel_busy_probe() {
  int thresh = hydra.radio().interferenceThreshold();
  if (thresh != 0 && radio_driver.getCurrentRSSI() > radio_driver.getNoiseFloor() + thresh) {
    return TXWAIT_RSSI;
  }
  if (!hydra.radio().cadEnabled()) return TXWAIT_RX_PACKET;
  // Only reached with CAD on, so the extra scan this costs is one the node was
  // already paying for on every send attempt.
  return radio_driver.isChannelActive() ? TXWAIT_CAD : TXWAIT_RX_PACKET;
}

HydraNode::HydraNode() : _core(radio_driver), _fs(NULL) {
  memset(_cfg, 0, sizeof(_cfg));
  _slots[0] = &_slot0;
  _cfg[0].type = SLOT_REPEATER;   // decision D: slot 0 is the repeater, always
#if HYDRA_NUM_CHAT_SLOTS > 0
  for (int i = 0; i < HYDRA_NUM_CHAT_SLOTS; i++) {
    _slots[i + 1] = &_chat[i];
    // Decision H: a fresh node is slot 0 only. Everything else is opt in, and
    // has no name until someone gives it one.
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
  // Slot 0 keeps the stock "_main" name, so a board reflashed from
  // simple_repeater to hydra comes back up as the same node to the mesh.
  //
  // Everything else is "_slotN", derived from the INDEX and nothing else. Not
  // the type, not the display name: this is the path a keypair is filed under,
  // and letting it move is how identities get lost (decision 7).
  if (idx == 0) StrHelper::strncpy(dest, "_main", sz);
  else snprintf(dest, sz, "_slot%d", idx);
}

void HydraNode::begin(FILESYSTEM* fs) {
  _fs = fs;
  loadSlotConfig();

  // Every slot's port is registered before anything is pumped, active or not,
  // so port index == slot index and the packet trace stays readable.
  for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
    int idx = _core.addPort(_slots[i]->port(), _cfg[i].type != SLOT_OFF);
    _core.setPortName(idx, i == 0 ? "repeater" : typeName(_cfg[i].type));
  }
  _core.setTxPowerControl(&s_tx_power);
  _core.setRadioReinit(hydra_radio_reinit);
  _core.setRxErrorCounter(rx_err_count);
  _core.setChannelBusyProbe(hydra_channel_busy_probe);
  // Routing is what the rest of the mesh depends on; chat traffic is not. Rank
  // rather than special-case slot 0, so a room server can be given the same
  // standing without the arbiter learning what a slot is.
  _core.setPortPriority(0, 0);
  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) _core.setPortPriority(i, 1);
  // Identities on one antenna are deaf to each other (half duplex), so slot 0
  // could never repeat for slot 1. Loopback makes each transmit look like a
  // receive to the sibling ports, which is what a second physical node in the
  // same room would experience.
  _core.setLoopback(true);
  _core.observer().setClock(&rtc_clock);   // so peers' advert clocks mean something

  // Seeded once, node-scoped: StdRNG is a facade over the global Arduino PRNG,
  // so per-slot seeding would only re-seed the same generator — and each call
  // pokes the shared transceiver for entropy.
  StdRNG seeder;
  seeder.begin(radio_driver.getRngSeed());

  startSlot(0);   // repeater: always on, whatever the config says
  NodePrefs* p = _slot0.prefs();
  // Duty cycle belongs to the antenna, so the whole node draws on one pool.
  // Slot 0's airtime_factor sets it; the chat slots' own Dispatcher budgets
  // still exist but are no longer what limits the node.
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
      // A slot refused at boot stays off for this boot but keeps its config, so
      // the reason stays visible in `slots` and nothing is silently rewritten.
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

  /* Decision G's reserve floor. Pin it first, then let the slot allocate what
     is left: a slot that starts has provably not eaten into the reserve, and
     one that cannot is refused NOW rather than returning NULL from allocNew()
     at 3am. Nothing here measures free heap — free-at-boot is not free-at-peak
     and this node is meant to sit unattended. */
  RamFloor floor(HYDRA_RAM_RESERVE);
  if (!floor.held()) return SLOT_ENABLE_NO_RAM;

  char id_name[16];
  slotIdName(idx, id_name, sizeof(id_name));
  IdentityStore store(*_fs, "");
  if (!_slots[idx]->begin(_fs, store, id_name, _cfg[idx].name, _cfg[idx].type)) {
    return SLOT_ENABLE_NO_RAM;   // begin() only fails when the heap refused
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
  if (idx <= 0 || idx >= HYDRA_NUM_SLOTS) return;   // slot 0 is not disableable
  // The identity stays constructed; only its port is silenced, so it stops
  // hearing and stops transmitting immediately and can be brought back without
  // touching the heap. Its keypair, ACL and contacts are untouched on disk.
  _slots[idx]->flushPendingWrites();
  _core.setPortActive(idx, false);
}

bool HydraNode::hasPendingWork() const {
  // EVERY active slot, not just slot 0. The powersave gate sleeps the board;
  // a slot whose queue is only consulted when slot 0 happens to be busy would
  // stall silently for as long as slot 0 stays quiet.
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
  // ORDER MATTERS (see SharedRadio.h): every identity consumes the current
  // frame before the arbiter fetches the next one.
  for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
    if (_core.portActive(i)) _slots[i]->loop();
  }
  _core.pump();
#ifdef LORA_WATCHDOG_MS
  _lora_wd.loop();   // paces itself, see LoraWatchdog::CHECK_EVERY_MS
#endif
}

// ---------------------------------------------------------------- slot config

// Deliberately a fixed-size binary record and not JSON: it is read once at boot
// before any identity exists, and prefs.json is already owned by slot 0's
// CommonCLI.
//
// SAFE MODE. Every failure path here lands on "plain repeater, other slots off"
// (decision D): a missing file returns early, a bad magic keeps the defaults, a
// short read breaks and keeps what came before it, and a type this build does
// not implement maps to SLOT_OFF rather than being reinterpreted. A stored name
// that would not pass the CLI's own check disables the slot too, so the "no
// nameless identity" rule survives a reboot and a corrupt file.
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
    /* v1 had no names, only a type byte per slot. An enabled v1 slot is already
       on the mesh under the name ChatSlot used to synthesise, so migrate to
       exactly that string rather than disabling a working identity. */
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
    // A slot count that shrank between builds leaves stale bytes; a partial
    // record at the tail must not half-apply.
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
  // Decision G asks for headroom here. This is a probe, not a free-heap
  // measurement, and nothing sizes itself from it: it is the largest single
  // block the allocator would still hand out right now.
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
    /* Decision E. This is a map of who can hear whom across the neighbourhood,
       assembled from third parties who never agreed to be in it, so it does not
       go out over the air — the same rule upstream applies to `get acl` and
       `get prv.key`. Physical access to the console is the authentication. */
    if (sender_timestamp != 0) {
      StrHelper::strncpy(reply, "ERR: peers is console-only - it describes third parties", reply_sz);
      return;
    }
    reportPeers(reply, reply_sz);
    return;
  }
  if (strcmp(command, "stats-shared") == 0) {   // the arbiter's view, node-wide
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
  if (strcmp(command, "trace") == 0) {          // the packet trace, newest last
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
    // Sized out, not broken: PKT_TRACE_ENTRIES is 0 in shipped builds because
    // the ring is 10.9 KB of RAM at the full depth.
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

  // Unqualified: slot 0. A hydra node still answers the whole repeater CLI, and
  // that fallthrough IS the node namespace — the radio, the watchdogs, the LED
  // and the board all hang off slot 0's prefs because there is one of each.
  _slot0.handleCommand(sender_timestamp, command, reply, reply_sz);
}

// -------------------------------------------------------------- `slot N ...`

void HydraNode::handleSlotCommand(int idx, uint32_t sender_timestamp, char* arg,
                                  char* reply, size_t reply_sz) {
  // Slot 0 is the repeater and its CLI is the stock one, qualified or not.
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
    /* A slot that has already run this boot has a mesh object built for the
       type it started as, and `slot N off` only silences the port. type() is
       SLOT_OFF until begin() runs, so this catches the stopped case too — which
       is the one that would otherwise come back up as the wrong thing. */
    if (_slots[idx]->type() != SLOT_OFF && _slots[idx]->type() != want) {
      snprintf(reply, reply_sz, "ERR: slot %d already ran as %s this boot - reboot to retype",
               idx, typeName(_slots[idx]->type()));
      return;
    }
    /* Decision H: the name gate runs BEFORE anything is persisted or started,
       so a refused enable leaves no trace and there is never an identity
       adverting with nothing to call itself. */
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
  /* Decision 8. A slot asking for a radio parameter is refused rather than
     served: there is one transceiver, and slot 3 retuning it would take every
     other identity off the air with it. */
  if (slotVerbIsNodeLevel(arg)) { StrHelper::strncpy(reply, slotNodeLevelError(), reply_sz); return; }

  if (strncmp(arg, "name ", 5) == 0) {
    const char* v = arg + 5;
    if (!slotNameValid(v)) {
      snprintf(reply, reply_sz, "ERR: bad name (1-%d chars, no [ ] \\ : , ? *)", SLOT_NAME_MAX - 1);
      return;
    }
    StrHelper::strncpy(_cfg[idx].name, v, sizeof(_cfg[idx].name));
    saveSlotConfig();
    _slots[idx]->setName(_cfg[idx].name);   // live: the next advert carries it
    snprintf(reply, reply_sz, "OK - slot %d is \"%s\"", idx, _cfg[idx].name);
    return;
  }
  if (strncmp(arg, "prv.key ", 8) == 0) {
#if ENABLE_PRIVATE_KEY_IMPORT
    setSlotPrivateKey(idx, arg + 8, reply, reply_sz);
#else
    // Upstream gates key import behind this flag and comments it "comment these
    // out for more secure firmware". With it off the command does not exist.
    StrHelper::strncpy(reply, "ERR: unknown setting", reply_sz);
#endif
    return;
  }
  if (strncmp(arg, "advert.interval ", 16) == 0) {
    // Same floor and ceiling as CommonCLI's node-level advert.interval: 0 is
    // "never", and anything else has to be at least an hour apart. One antenna
    // and N identities means N times the adverts if the floor is not kept.
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
    // Console only, like upstream's `get prv.key`: it IS the identity. Worth
    // having on a slot whose key was generated here, because otherwise the only
    // copy is on a filesystem nobody has backed up.
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
/* Moving an existing identity onto a slot.

   THIS WRITES A KEYPAIR TO FLASH, which is the operation this project has lost
   identities to before (see slot_types.h on origin/time-converge). Three rules,
   all of them here on purpose:

   1. The storage name comes from slotIdName(), i.e. from the slot INDEX, and is
      the same call begin() loads with. There is no second convention, and no
      path that depends on the slot's type or its display name.
   2. The slot must be OFF. A running identity has contacts, an ACL and a mesh
      object keyed to the old key; swapping underneath them would leave a node
      that is half one identity and half another. Off means the only state is
      the file, and begin() reads it fresh.
   3. Write, then READ BACK and compare before reporting success. A truncated or
      failed LittleFS write otherwise reports OK and is discovered at the next
      boot, with the old key already gone. */
bool HydraNode::setSlotPrivateKey(int idx, const char* hex, char* reply, size_t reply_sz) {
  /* Rule 2. type() is SLOT_OFF only while begin() has never run, so this also
     refuses a slot that was started and then switched off: its mesh object,
     contacts and ACL are still keyed to the old identity in RAM, and `slot N
     on` would not re-read the file. */
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
  slotIdName(idx, id_name, sizeof(id_name));   // rule 1: index, nothing else
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
