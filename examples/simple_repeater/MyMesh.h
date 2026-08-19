#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
  using File = fs::File;
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_BLE_BRIDGE
#include "helpers/bridges/BLEBridge.h"
#define WITH_BRIDGE
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/BatteryEstimator.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/RegionMap.h>
#include <helpers/RoutingPolicy.h>
#include "RateLimiter.h"

#ifdef WITH_BRIDGE
extern AbstractBridge* bridge;
#endif

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
};

#ifndef FIRMWARE_BUILD_DATE
  /* build.sh sets this; a plain `pio run` does not, and the literal that used to
     sit here then reported a date the image was NOT built on. That is worse than
     no date at all: it reads as confirmation while carrying no information, so a
     freshly flashed node looks identical to one running a week-old image.
     __DATE__ costs a rebuild of this translation unit and cannot go stale. */
  #define FIRMWARE_BUILD_DATE   __DATE__
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.17.1"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

#ifdef LOOP_WATCHDOG_MS
  #include <helpers/nrf52/LoopWatchdog.h>
#endif
#if WITH_MESH_OBSERVER
  #include "helpers/MeshObserver.h"
#endif
#if WITH_BLE_CLI
  #include <helpers/BaseSerialInterface.h>
  #include <helpers/nrf52/BleStack.h>
  #include <helpers/nrf52/SerialBLEInterface.h>
#endif

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  uint8_t reply_path_len;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  RateLimiter discover_limiter, anon_limiter;
  uint32_t pending_discover_tag;
  unsigned long pending_discover_until;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#elif defined(WITH_BLE_BRIDGE)
  BLEBridge bridge;
#endif

  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  File openAppend(const char* fname);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);

#if WITH_MESH_OBSERVER
  // Passive metrics fed from the raw receive/transmit hooks. Costs a few KB of
  // RAM and nothing on air; off unless the build asks for it.
  MeshObserver _obs;

  /* Clock convergence: steer our clock towards what the neighbourhood says the
     time is. The observer supplies the estimate; the policy for acting on it
     lives here -- see maybeConvergeClock(). Off until "clocks on". */
  static const uint32_t CLOCK_CONVERGE_INTERVAL_MS = 5UL * 60UL * 1000UL;
  static const int32_t  CLOCK_DEADBAND_S = 2;     // agreement to the second is enough
  static const int32_t  CLOCK_SLEW_MAX_S = 2;     // per interval, either direction
  static const int32_t  CLOCK_STEP_MIN_S = 30;    // below this, never worth a jump
  static const uint8_t  CLOCK_STEP_MIN_AGREE = 80;
  /* A survey of a real 407-node mesh put the false-fire rate of the step gate
     at 0.2% of rounds with eight sources but 4.3% with four, so a step needs a
     real quorum. */
  static const uint8_t  CLOCK_STEP_MIN_SOURCES = 6;
  /* And the survivors must actually agree with each other, not merely all
     survive clipping. 15s is about twice the 7s MAD the survey mesh runs at,
     which lets a genuinely-wrong node step within about three rounds while
     still refusing a population split between two beliefs -- that case reports
     100% agreement on a midpoint nobody holds, with a spread of 150s. */
  static const int32_t  CLOCK_STEP_MAX_SPREAD_S = 15;
  static const uint32_t CLOCK_HOLDOVER_MS = 6UL * 60UL * 60UL * 1000UL;
  /* Slewing needs a quality gate of its own: on hardware a node computed +50s
     from seven scattered multi-hop sources with a spread of 62s, and nothing
     stopped the slew from walking a known-good clock 50s away two seconds at a
     time. When sources disagree this much the median is not evidence. */
  static const int32_t  CLOCK_MAX_SPREAD_TO_ACT_S = 30;

  /* None of that caution applies when our clock was never set. These boards
     have no hardware RTC, so every reboot lands them back on 15 May 2024 --
     and until they leave it their adverts carry timestamps the rest of the mesh
     rejects outright as replays, which makes the node not merely wrong but
     invisible. Nothing to protect, so check often and take the first credible
     consensus whole. */
  static const uint32_t CLOCK_CONVERGE_FAST_MS = 30UL * 1000UL;
  static const uint8_t  CLOCK_UNSET_MIN_SOURCES = 2;
  /* Measured on hardware: at 600s this accepted a two-source consensus and
     landed 113s off true UTC, and because the neighbourhood's ordinary spread
     then sat at 398s the normal path refused to refine it -- so the node stayed
     wrong. The escape only has to be close enough for ordinary steering to take
     over, so it is worth waiting a little longer for a tighter sample. */
  static const int32_t  CLOCK_UNSET_MAX_SPREAD_S = 120;

  /* Mirrors _prefs.clock_converge, which is the persisted authority. */
  uint32_t _next_clock_converge_ms = 0;
  uint32_t _clock_extern_set_ms = 0;
  bool     _clock_ever_set = false;
  int32_t  _last_clock_adj_s = 0;
  uint32_t _clock_steps = 0;
  uint32_t _clock_slews = 0;
  bool clockIsUnset() const;
  void maybeConvergeClock();
#endif

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  bool getCADEnabled() const override {
    return _prefs.cad_enabled;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void sendNodeDiscoverReq();
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  /* Deferred, not immediate. A prefs save is remove-then-rewrite of the whole
     file, and under BLE load it stalls this loop for around 1.6 seconds --
     measured, and dominated by SoftDevice flash arbitration rather than by
     LittleFS, since erase and write can only proceed in radio-idle slots.
     During that stall the bridge arbiter stops being driven and the connectable
     advert can be left held.

     There are 56 savePrefs() call sites in the CLI, one per setting, so
     configuring a node runs that stall once per command. Coalescing here costs
     one flag and turns a burst of settings into a single write. */
  static const uint32_t PREFS_SETTLE_MS = 2000;
  unsigned long _prefs_dirty_ms = 0;

  void savePrefs() override {
    _prefs_dirty_ms = millis();
  }
  /** Write now if anything is pending -- before a reboot, poweroff or OTA,
   *  where a deferred write would otherwise be lost. */
  void flushPrefs() override {
    if (_prefs_dirty_ms == 0) return;
    _prefs_dirty_ms = 0;
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
#if defined(WITH_BLE_BRIDGE)
  void formatBridgeReply(char *reply, const char* what) override;
#if WITH_MESH_OBSERVER
  void formatObserverReply(char *reply, const char* what) override;
  void onClockSetExternally() override;
#endif
#endif
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);

  /* Loop iterations, sampled per second. The Arduino task runs at
     TASK_PRIO_LOW and every BLE report preempts it, so the rate is a
     whole-system proxy for what the radio side is stealing -- and unlike
     FreeRTOS run-time stats it costs one increment and needs no core patch. */
  uint32_t _loop_iters = 0, _loop_rate = 0;
  unsigned long _loop_rate_ms = 0;
  /* Longest gap ever seen between two loop() entries. This is the number a
     hardware watchdog timeout has to clear: the WDT on this part cannot be
     stopped once started, so any legitimate blocking operation longer than the
     timeout becomes a reset loop. Measuring the worst case beats auditing for
     it -- filesystem writes, LoRa transmit and BLE work all block here. */
  unsigned long _loop_gap_max_ms = 0, _loop_last_ms = 0;

  /* Charge/discharge inference from voltage alone -- there is no current
     sensing on this hardware. See BatteryEstimator.h. */
  BatteryEstimator _batt;
#ifdef LOOP_WATCHDOG_MS
  /* Latches on the first loop pass, where the watchdog drops from the boot
     limit to the runtime limit. See loop(). */
  bool _wdog_tightened = false;
#endif

  /* LoRa watchdog. Both repeaters have been found with a completely dead radio
     -- zero packets sent or received for over an hour, correct config, repeat
     on -- and a reboot did not clear it, so nothing short of intervention got
     them back on the air. Nothing noticed, because a silent band and a dead
     radio look identical from the outside.

     They are distinguishable if we make traffic ourselves: transmitting is
     always possible, so after a long idle period the node sends one zero-hop
     advert and watches whether its own transmit airtime moves. That separates
     "nobody is talking" from "this radio is not working" without waiting for
     someone else to speak, which on a quiet band may be never. */
  static const uint32_t LORA_IDLE_MS = 15UL * 60UL * 1000UL;
  static const uint32_t LORA_SELFTEST_GRACE_MS = 30000;
  static const uint32_t LORA_CHECK_EVERY_MS = 30000;
  enum LoraWd : uint8_t { LORA_WD_IDLE = 0, LORA_WD_TESTING, LORA_WD_REINITED };

  unsigned long _lora_activity_ms = 0;   // when air time last moved
  unsigned long _lora_last_air = 0;      // tx+rx air time at that moment
  unsigned long _lora_next_check_ms = 0;
  unsigned long _lora_test_started_ms = 0;
  unsigned long _lora_test_air = 0;
  uint8_t  _lora_wd_state = LORA_WD_IDLE;
  uint32_t _lora_reinits = 0;
  void loraWatchdog();

#if WITH_BLE_CLI
private:
  // A local, high-bandwidth diagnostic port. Metrics over LoRa are capped at a
  // ~160-byte reply and cost airtime on a congested band; over BLE they cost
  // nothing. Reuses the companion's SerialBLEInterface unchanged, which also
  // brings Adafruit's DFU service -- so this is the firmware-update path too,
  // and on a node with no USB attached it is the ONLY way back in.
  BaseSerialInterface* _ble = nullptr;
  uint32_t _ble_pin = 0;
  uint32_t _ble_seq = 0;      // monotonic, for the CLI's replay guard
  void bleLoop();
public:
  // The PIN is generated per boot, so a stolen pairing cannot be replayed after
  // a restart and there is no shipped default to look up.
  void startBLE(SerialBLEInterface& ble, const char* name_prefix, char* name);
  uint32_t blePin() const { return _ble_pin; }
  void formatBleReply(char *reply) override;
  // handleCommand above is public; restore that so loop() and friends below
  // keep the access they had before this block was inserted.
#endif
  void loop();

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
    if (enable == bridge.isRunning()) return;
    if (enable)
    {
      bridge.begin();
    }
    else 
    {
      bridge.end();
    }
  }

  void restartBridge() override {
    if (!bridge.isRunning()) return;
    bridge.end();
    bridge.begin();
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

  bool setRxBoostedGain(bool enable) override;

  #if defined(USE_LR2021)
  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) override;
  #endif

};
