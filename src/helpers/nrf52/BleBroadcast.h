#pragma once

#include <stdint.h>
#include <stddef.h>
#include <bluefruit.h>

/**
 * @brief  Connectionless many-to-many datagrams over BLE 5 extended advertising.
 *
 * This is the nRF52 answer to ESP-NOW: no pairing, no connections, no peer
 * state. Every node in range that is scanning hears every datagram. It exists
 * so a board with no WiFi (RAK3401 and friends) can run the same bridge
 * topology the ESP32 boards get from ESPNowBridge.
 *
 * Why extended advertising and not legacy: a legacy advert carries 31 bytes,
 * of which about 24 would survive our framing, so a full MeshCore packet would
 * need eleven fragments with no acknowledgement anywhere -- delivery decays as
 * (1-p)^11 and collapses as soon as two nodes talk at once. An extended
 * advertising PDU carries 255, so a packet always fits in exactly one
 * transmission and there is no reassembly to get wrong.
 *
 * Payload is wrapped in one Manufacturer Specific Data AD structure so the air
 * format stays well-formed BLE and every other scanner in earshot ignores it.
 *
 * NOT a general-purpose BLE object: exactly one instance can exist, because
 * the SoftDevice has exactly one advertising set and one scanner.
 */
class BleBroadcast {
public:
  /** Called from the SoftDevice event handler for each accepted datagram. */
  typedef void (*rx_handler_t)(const uint8_t* payload, uint8_t len,
                               const uint8_t peer_addr[6], uint8_t peer_addr_type,
                               int8_t rssi);

  /** Chained raw-event callback -- see begin(). */
  typedef void (*event_chain_t)(ble_evt_t* evt);

  /**
   * AD framing overhead: length(1) + type(1) + company ID(2).
   * The SoftDevice will accept 255 bytes of advertising data in an extended
   * PDU, so this is what is left for a caller.
   */
  static const uint8_t AD_OVERHEAD = 4;
  static const uint8_t MAX_PAYLOAD = 255 - AD_OVERHEAD;   // 251

  /** Datagrams buffered for transmission. Each burst occupies the radio for
   *  tens of milliseconds, so a little queueing absorbs bursty senders. */
  static const uint8_t QUEUE_SIZE = 4;

  BleBroadcast() {}

  /**
   * @brief  Bring up the BLE stack, for a host that has no other BLE user.
   *
   * A repeater with no companion or CLI interface has nobody to call
   * Bluefruit.begin() for it. Call this once before begin() in that case. It is
   * idempotent, so a host that already has BLE up is unaffected.
   *
   * @param name  GAP device name, or NULL to leave the default alone.
   */
  static bool initStack(const char* name = nullptr);

  /**
   * @brief  Start advertising-based broadcast.
   *
   * MUST be called after the BLE stack is up (Bluefruit.begin()) and after any
   * other subsystem that installs a raw event callback -- see the `chain`
   * parameter.
   *
   * @param company_id  Manufacturer ID used to tag and filter our adverts.
   * @param handler     Invoked for each accepted datagram, from SoftDevice
   *                    event context. Keep it short.
   * @param chain       Bluefruit.setEventCallback() is a single slot, and on a
   *                    repeater SerialBLEInterface has already claimed it (it
   *                    needs CONN_PARAM_UPDATE_REQUEST or the CLI connection
   *                    eventually drops). We take the slot and forward every
   *                    event we do not consume to this. Pass the previous
   *                    handler, or NULL if there was none.
   * @returns false if the SoftDevice refused, in which case nothing is running.
   */
  bool begin(uint16_t company_id, rx_handler_t handler, event_chain_t chain);

  /** Stop scanning, abandon any in-flight burst, restore connectable adverts. */
  void end();

  bool isRunning() const { return _running; }

  /**
   * @brief  Queue one datagram for broadcast.
   * @returns false if oversize or the queue is full (caller should drop).
   */
  bool send(const uint8_t* payload, uint8_t len);

  /** Drive the advertising-set arbiter. Call from the main loop. */
  void loop();

  /** Radio time is currently ours rather than the connectable advert's. */
  bool isBursting() const { return _bursting; }

  /**
   * @brief  True while transmission still needs loop() to be called on time.
   *
   * Receiving survives sleep -- SoftDevice radio events wake the CPU -- but the
   * burst arbiter works to millis() deadlines, so a sleeping node with a queued
   * datagram would stall until some unrelated interrupt happened along.
   */
  bool hasPendingWork() const { return _bursting || _queue_len > 0; }

  /** Advertising events per datagram, and how long a datagram waits before its
   *  burst starts. Both are applied to the NEXT burst, so changing them takes
   *  effect without a restart. */
  void setAdvRepeat(uint8_t evts) {
    uint8_t n = (evts < 1) ? 1 : evts;
    if (n == _adv_repeat) return;
    _adv_repeat = n;
    _rescan_needed = true;    // the scan cycle is derived from this
  }
  void setTxHoldMs(uint16_t ms) { _tx_hold_ms = ms; }
  /**
   * @brief  Only generate reports for these addresses, in the link layer.
   *
   * Worth far more than the CPU it saves. The SoftDevice PAUSES scanning on
   * every completed report and does not resume until the application hands the
   * buffer back, so each foreign beacon costs listening time as well as a
   * high-priority task wakeup -- and on a live node 82% of all reports were
   * other people's. Filtering by address happens before any of that.
   *
   * BLE can only filter on address, never on content, so there is no way to ask
   * for "company ID 0xFFFF" -- hence learning peers first and whitelisting them
   * afterwards, with discovery windows to find new ones.
   */
  void setWhitelist(const ble_gap_addr_t* addrs, uint8_t n);
  void setScanFilter(bool on) {
    if (on == _filter_enabled) return;
    _filter_enabled = on;
    _rescan_needed = true;
  }
  /** True while deliberately listening to everyone, to discover new peers. */
  bool inDiscovery() const { return _discovery_until_ms != 0; }

  void setScanDuty(uint8_t pct) {
    uint8_t d = pct < 25 ? 25 : (pct > 100 ? 100 : pct);
    if (d == _scan_duty) return;
    _scan_duty = d;
    _rescan_needed = true;
  }

  /**
   * @brief  CPU spent ingesting advertising reports, in microseconds.
   *
   * The only part of scanning we control. Note what this does NOT include: the
   * link layer still receives and decodes every advert on air even when the
   * whitelist rejects it, so filtering saves this figure but not radio time or
   * radio current. Anyone reading a battery number off this should know that.
   *
   * Measured around the report handler, which runs at TASK_PRIO_HIGH and
   * preempts everything MeshCore does -- so this is also the figure for how
   * much the BLE side steals from LoRa.
   */
  uint32_t reportCpuUs() const { return _report_cpu_us; }
  uint32_t reportCount() const { return _report_count; }
  /** Mean RSSI over all reports seen, as a proxy for ambient BLE activity --
   *  there is no way to sample a true noise floor while the SoftDevice scans. */
  int8_t meanReportRssi() const {
    return _report_count ? (int8_t)(_rssi_sum / (int32_t)_report_count) : 0;
  }

  uint32_t numSent() const { return _num_sent; }
  uint32_t numRecv() const { return _num_recv; }
  uint32_t numTxDropped() const { return _num_tx_dropped; }

private:
  /* Advertising interval for a burst, in 625us units. 20ms is the shortest
     interval permitted for a non-connectable extended advert. */
  static const uint32_t ADV_INTERVAL = 32;

  /* Repeat each datagram over this many advertising events. A scanner running
     at less than 100% duty cycle would otherwise miss whole datagrams; three
     chances spread over ~60ms comfortably covers our own scan duty cycle. */
  static const uint8_t MAX_ADV_EVTS = 3;

  /* Safety net: if ADV_SET_TERMINATED never arrives we would wedge holding the
     advertising set forever, and the diagnostic port would never come back. */
  static const uint32_t BURST_TIMEOUT_MS = 400;

  /* Minimum idle between bursts, so back-to-back traffic cannot completely
     monopolise the single advertising set. */
  static const uint32_t BURST_GAP_MS = 20;

  /* The SoftDevice has ONE advertising set (BLE_GAP_ADV_SET_COUNT_MAX == 1),
     so our broadcast and the connectable CLI/DFU advert cannot both run. We
     guarantee the connectable advert at least CONNECTABLE_MIN_MS of every
     CONNECTABLE_PERIOD_MS, otherwise a busy bridge would make the diagnostic
     port undiscoverable.

     What governs DISCOVERY is not the reserved fraction but how often a
     reserved window overlaps the scanning central's own window -- one long
     window every two seconds is easy to keep missing. Hence a 1s cycle with a
     third of it reserved, rather than 2s with 15%. Bridge throughput drops to
     about two thirds of its ceiling, still far above what a repeater generates.
     Override per build for a node that is bridge-heavy and never needs to be
     connected to. */
#ifndef BLE_BRIDGE_CONNECTABLE_PERIOD_MS
  #define BLE_BRIDGE_CONNECTABLE_PERIOD_MS 1000
#endif
#ifndef BLE_BRIDGE_CONNECTABLE_MIN_MS
  #define BLE_BRIDGE_CONNECTABLE_MIN_MS 350
#endif
  static const uint32_t CONNECTABLE_PERIOD_MS = BLE_BRIDGE_CONNECTABLE_PERIOD_MS;
  static const uint32_t CONNECTABLE_MIN_MS = BLE_BRIDGE_CONNECTABLE_MIN_MS;

  /* Scan duty cycle, as a fraction of the interval. Deliberately not 100% --
     the SoftDevice needs slack to service our own advertising bursts and any
     connection. */
  static const uint8_t SCAN_DUTY_DEFAULT = 80;   // percent

  /* The scan interval is DERIVED from the burst rather than fixed, so the
     copies of one datagram land on evenly spaced scan phases.
     
     It matters more than it looks. With a fixed 50ms interval and copies 20ms
     apart, the three copies fall at phases 0, 20 and 40 of a 50ms cycle -- and
     because phase wraps, the first and last are only 10ms apart, exactly the
     width of the blind gap. One badly placed gap could swallow BOTH, so a
     nominal 80% duty cycle could still lose two copies in three. Setting the
     interval to adv_repeat * ADV_INTERVAL makes the gaps uniform, and the blind
     window can then only ever cost one copy whatever the phase. */
  uint16_t scanIntervalUnits() const { return (uint16_t)(ADV_INTERVAL * _adv_repeat); }
  uint16_t scanWindowUnits() const {
    uint16_t iv = scanIntervalUnits();
    uint32_t w = (uint32_t)iv * _scan_duty / 100;
    if (w < 4) w = 4;                          // SoftDevice minimum
    if (w > iv) w = iv;                        // window may never exceed interval
    return (uint16_t)w;
  }

  struct Datagram {
    uint8_t len;
    unsigned long queued_ms;   // for the pre-burst hold
    uint8_t buf[MAX_PAYLOAD];
  };

  /** Build the manufacturer-data AD structure into _adv_buf and hand the set
   *  to the SoftDevice. Returns the raw sd_ble_gap_adv_set_configure() result. */
  uint32_t configureAdvSet(const uint8_t* payload, uint8_t len);

  bool startBurst(const Datagram& d);
  void finishBurst();
  void shiftQueueLeft();
  bool armScan(bool first);
  void onAdvReport(const ble_gap_evt_adv_report_t* report);

  static void onBLEEvent(ble_evt_t* evt);

  bool _running = false;
  bool _bursting = false;
  bool _restore_connectable = false;

  /* Whether anything else actually wants the advertising set. On a repeater
     with no CLI or companion interface nothing else advertises, so the
     connectable-window reservation below would just throttle us for nobody. */
  bool _shares_adv_set = false;

  uint16_t _company_id = 0;
  rx_handler_t _handler = nullptr;
  event_chain_t _chain = nullptr;

  uint8_t _adv_handle = 0;
  unsigned long _burst_started_ms = 0;
  unsigned long _next_burst_allowed_ms = 0;

  /* Rolling accounting for the connectable-advert guarantee. */
  unsigned long _window_started_ms = 0;
  uint32_t _burst_ms_in_window = 0;

  uint8_t _adv_repeat = MAX_ADV_EVTS;
  uint16_t _tx_hold_ms = 0;
  /* A whitelisted scanner is deaf to anyone it has not met, so it has to open
     its ears periodically or a new node could never join. Short and infrequent:
     the cost of a discovery window is a window's worth of foreign reports. */
  static const uint32_t DISCOVERY_PERIOD_MS = 60000;
  static const uint32_t DISCOVERY_WINDOW_MS = 4000;
  static const uint8_t  MAX_WHITELIST = 8;      // BLE_GAP_WHITELIST_ADDR_MAX_COUNT

  ble_gap_addr_t _wl[MAX_WHITELIST];
  uint8_t _wl_count = 0;
  bool _filter_enabled = false;
  unsigned long _discovery_until_ms = 0;
  unsigned long _next_discovery_ms = 0;
  bool useWhitelist() const {
    return _filter_enabled && _wl_count > 0 && _discovery_until_ms == 0;
  }

  uint8_t _scan_duty = SCAN_DUTY_DEFAULT;
  bool _rescan_needed = false;

  uint8_t _queue_len = 0;
  Datagram _queue[QUEUE_SIZE];

  /* The SoftDevice keeps a pointer to the advertising buffer for as long as
     the set is configured, so this must outlive each burst and must not be
     touched while one is in flight. */
  uint8_t _adv_buf[255];
  uint8_t _adv_len = 0;

  uint32_t _num_sent = 0;
  uint32_t _num_recv = 0;
  uint32_t _num_tx_dropped = 0;
  uint32_t _report_cpu_us = 0;
  uint32_t _report_count = 0;
  int32_t  _rssi_sum = 0;
};

#if BLE_BROADCAST_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define BLE_BCAST_DEBUG_PRINTLN(F, ...) Serial.printf("BleBroadcast: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_BCAST_DEBUG_PRINTLN(...) {}
#endif
