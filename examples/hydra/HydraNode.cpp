#include "HydraNode.h"

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
  hydra.repeater().mesh().flushPendingWrites();
  board.reboot();
}
#endif

static uint32_t rx_err_count() { return radio_driver.getPacketsRecvErrors(); }

// The sender's coding rate, and what a frame of that length actually cost the
// channel at it. The core sees only a mesh::Radio, so the driver is reached
// through these rather than by knowing what it is.
static uint8_t rx_coding_rate() { return radio_driver.getLastRxCodingRate(); }
static uint32_t rx_airtime_at_cr(int len_bytes, uint8_t cr) {
  return radio_driver.getEstAirtimeForCR(len_bytes, cr);
}

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
  _slots[0] = &_slot0;
  _cfg[0] = SLOT_REPEATER;
#if HYDRA_NUM_CHAT_SLOTS > 0
  for (int i = 0; i < HYDRA_NUM_CHAT_SLOTS; i++) {
    _slots[i + 1] = &_chat[i];
    _cfg[i + 1] = SLOT_OFF;
  }
#endif
}

const char* HydraNode::typeName(SlotType t) {
  switch (t) {
    case SLOT_REPEATER: return "repeater";
    case SLOT_CHAT:     return "chat";
    default:            return "off";
  }
}

void HydraNode::slotIdName(int idx, char* dest, size_t sz) {
  // Slot 0 keeps the stock "_main" name, so a board reflashed from
  // simple_repeater to hydra comes back up as the same node to the mesh.
  if (idx == 0) StrHelper::strncpy(dest, "_main", sz);
  else snprintf(dest, sz, "_slot%d", idx);
}

void HydraNode::begin(FILESYSTEM* fs) {
  _fs = fs;
  loadSlotConfig();

  // Every slot's port is registered before anything is pumped, active or not,
  // so port index == slot index and the packet trace stays readable.
  for (int i = 0; i < HYDRA_NUM_SLOTS; i++) {
    int idx = _core.addPort(_slots[i]->port(), _cfg[i] != SLOT_OFF);
    _core.setPortName(idx, i == 0 ? "repeater" : "chat");
  }
  _core.setTxPowerControl(&s_tx_power);
  _core.setRadioReinit(hydra_radio_reinit);
  _core.setRxErrorCounter(rx_err_count);
  _core.setRxCodingRateFn(rx_coding_rate);
  _core.setRxAirtimeFn(rx_airtime_at_cr);
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
    if (_cfg[i] == SLOT_OFF) continue;
    if (startSlot(i)) {
      _slots[i]->port().setPortTxPower(p->tx_power_dbm);
      _core.setPortIdentity(i, _slots[i]->identity().pub_key);
    }
  }
}

bool HydraNode::startSlot(int idx) {
  if (idx < 0 || idx >= HYDRA_NUM_SLOTS) return false;
  char id_name[16];
  slotIdName(idx, id_name, sizeof(id_name));
  IdentityStore store(*_fs, "");
  if (!_slots[idx]->begin(_fs, store, id_name)) return false;
  _core.setPortActive(idx, true);

  Serial.printf("hydra: slot %d (%s) id ", idx, typeName(_slots[idx]->type()));
  mesh::Utils::printHex(Serial, _slots[idx]->identity().pub_key, PUB_KEY_SIZE);
  Serial.println();
  return true;
}

void HydraNode::stopSlot(int idx) {
  if (idx <= 0 || idx >= HYDRA_NUM_SLOTS) return;   // slot 0 is not disableable
  // The identity stays constructed; only its port is silenced, so it stops
  // hearing and stops transmitting immediately and can be brought back without
  // touching the heap. Its keypair and contacts are untouched on the filesystem.
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

// Deliberately a fixed-size binary record and not JSON: it is HYDRA_NUM_SLOTS
// bytes, it is read once at boot before any identity exists, and prefs.json is
// already owned by slot 0's CommonCLI.
void HydraNode::loadSlotConfig() {
  if (!_fs->exists(HYDRA_SLOT_CFG_FILE)) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(HYDRA_SLOT_CFG_FILE, "r");
#else
  File f = _fs->open(HYDRA_SLOT_CFG_FILE);
#endif
  if (!f) return;
  uint8_t hdr[2];
  if (f.read(hdr, 2) == 2 && hdr[0] == 'H' && hdr[1] == 1) {
    for (int i = 1; i < HYDRA_NUM_SLOTS; i++) {
      uint8_t t = SLOT_OFF;
      if (f.read(&t, 1) != 1) break;
      // A slot count that shrank between builds leaves stale bytes; a type this
      // build does not implement (room) must not silently become something else.
      _cfg[i] = (t == SLOT_CHAT) ? SLOT_CHAT : SLOT_OFF;
    }
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
  uint8_t hdr[2] = { 'H', 1 };
  f.write(hdr, 2);
  for (int i = 1; i < HYDRA_NUM_SLOTS; i++) {
    uint8_t t = (uint8_t)_cfg[i];
    f.write(&t, 1);
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
    int w = snprintf(reply + n, reply_sz - n, " [%d]%s%s %s rx=%u tx=%u;", i,
                     typeName(_cfg[i]), live ? "" : "(down)", key,
                     (unsigned)_core.portRxCount(i), (unsigned)_core.portTxCount(i));
    if (w < 0) break;
    n += w;
  }
}

void HydraNode::handleCommand(char* command, char* reply, size_t reply_sz) {
  reply[0] = 0;

  if (strcmp(command, "slots") == 0) {
    formatSlotTable(reply, reply_sz);
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
    if (strcmp(arg, "on") == 0 || strcmp(arg, "chat") == 0) {
      if (idx == 0) { StrHelper::strncpy(reply, "slot 0 is always on", reply_sz); return; }
      _cfg[idx] = SLOT_CHAT;
      saveSlotConfig();
      if (!startSlot(idx)) { StrHelper::strncpy(reply, "ERR: slot failed to start", reply_sz); return; }
      _core.setPortIdentity(idx, _slots[idx]->identity().pub_key);
      _slots[idx]->port().setPortTxPower(_slot0.prefs()->tx_power_dbm);
      snprintf(reply, reply_sz, "OK - slot %d chat", idx);
      return;
    }
    if (strcmp(arg, "off") == 0) {
      if (idx == 0) { StrHelper::strncpy(reply, "slot 0 is always on", reply_sz); return; }
      _cfg[idx] = SLOT_OFF;
      saveSlotConfig();
      stopSlot(idx);
      snprintf(reply, reply_sz, "OK - slot %d off", idx);
      return;
    }
    _slots[idx]->handleCommand(arg, reply, reply_sz);
    return;
  }

  // Unqualified: slot 0. A hydra node still answers the whole repeater CLI.
  _slot0.handleCommand(command, reply, reply_sz);
}
