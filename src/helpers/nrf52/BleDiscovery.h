#pragma once

#include <stdint.h>
#include <stddef.h>
#include <bluefruit.h>

/* The beacon record and the group marker are part of the bridge's wire format,
   so they live beside the frame codec. That header needs no BLE stack and no
   Arduino, which is how the host tests reach them. */
#include "helpers/bridges/BleBridgeFrame.h"

/**
 * @brief  How two bridge nodes find each other over BLE, and stay findable.
 *
 * BleLink carries the data but cannot find a peer. This class finds one. It
 * advertises a small beacon that names our bridge group, it scans for the same
 * beacon from other nodes, and it reports each address that matches.
 *
 * The beacon is a LEGACY advert, not an extended one. A legacy advert carries
 * 31 bytes, which is enough for the beacon and is what every scanner and every
 * phone can read. The node sends it about every 150ms.
 *
 * Beacon record, inside one Manufacturer Specific Data AD structure:
 *   [2] company ID, little-endian
 *   [1] record version
 *   [2] group marker, little-endian
 *   [1] battery, in decivolts (3.7V is 37)
 *
 * The GROUP MARKER is what stops a node from dialling every MeshCore device in
 * range. The bridge derives it from the group secret and gives it to begin().
 * The marker is public, and an attacker in radio range can read it and repeat
 * it. The marker is thus a filter, not a trust decision. The bridge
 * authenticates the peer on the first frame over the link. See BLEBridge.
 *
 * ONE advertising set exists in the SoftDevice (BLE_GAP_ADV_SET_COUNT_MAX is
 * 1), and a node needs both a connectable advert and a beacon. This class is
 * thus the only owner of that set, and it runs one advert at a time:
 *
 *  - A CONNECTABLE advert while a peripheral slot is free. A peer can dial us,
 *    and a person can reach the CLI and the DFU port. The beacon record rides
 *    on it, so one advert does both jobs.
 *  - A NON-CONNECTABLE presence beacon while no peripheral slot is free. The
 *    SoftDevice refuses to start a connectable advert with no free connection
 *    slot, so a node with a peer link vanishes from every scanner. Both
 *    repeaters showed this fault: the one that ACCEPTED the link went invisible,
 *    and the two swapped roles each time the link came back. A non-connectable
 *    advert needs no connection slot, so it always starts. A node that is
 *    visible but not connectable is far better than a node that is absent.
 *
 * EXACTLY ONE instance can exist, because the SoftDevice has one advertising
 * set and one scanner.
 *
 * This class must be the only BLE advertiser on the build. It drives
 * advertising set 0 directly and turns off Bluefruit's restart-on-disconnect,
 * because two owners of one set fight and the node then stops advertising.
 */
class BleDiscovery {
public:
  /** Called for each beacon that carries our company ID and our group marker.
   *  Runs on the Bluefruit callback task. Keep it short. */
  typedef void (*peer_handler_t)(const ble_gap_addr_t& addr, int8_t rssi);

  /** company(2) + version(1) + marker(2) + battery decivolts(1). */
  static const uint8_t BEACON_LEN = BleBridgeFrame::BEACON_LEN;

  BleDiscovery() {}

  /**
   * @param company_id     the Manufacturer ID that tags our beacon.
   * @param group_marker   2 bytes derived from the group secret.
   * @param handler        called for each peer beacon that matches.
   * @returns false if the SoftDevice refused. Nothing runs in that case.
   */
  bool begin(uint16_t company_id, uint16_t group_marker, peer_handler_t handler);

  /** Stop the scanner and the advert. */
  void end();

  bool isRunning() const { return _running; }

  /** Drive the advert arbiter, the scanner and the liveness check. Call this
   *  from the main loop. */
  void loop();

  /**
   * @brief  Ask for a scanner restart on the next loop pass.
   *
   * A connect attempt stops the scanner. The SoftDevice documents that
   * sd_ble_gap_connect() stops the scan, and nothing starts it again. A node
   * that dials a peer thus goes deaf at that moment. This is what happened
   * here: the node that dialled lost its scanner, and the node that accepted
   * kept hers.
   */
  void requestRescan() { _scan_armed = false; }

  /** Battery voltage for the beacon. Safe to call every loop pass. The advert
   *  is rebuilt only when the decivolt value changes. */
  void setBattery(uint16_t batt_mv) { _batt_dv = (uint8_t)(batt_mv / 100); }

  /**
   * @brief  CPU spent on advert reports, in microseconds.
   *
   * This is the only part of the scan that we control. It does NOT include
   * radio time: the link layer still receives and decodes every advert on the
   * air. Anyone who reads a battery figure from this number must know that.
   */
  uint32_t reportCpuUs() const { return _report_cpu_us; }
  uint32_t reportCount() const { return _report_count; }
  /** Beacons that carry our company ID and our group marker. */
  uint32_t numBeacons() const { return _num_beacons; }
  /** Adverts with our company ID but a different marker, or no marker at all.
   *  0xFFFF is the shared development ID of the Bluetooth SIG, so other
   *  people's beacons land here. A large count is ambient traffic, not a
   *  fault. */
  uint32_t numForeignBeacons() const { return _num_foreign_beacons; }
  /** Times the receive path went quiet and needed a restart. */
  uint32_t numRecoveries() const { return _num_recoveries; }
  /** Milliseconds since the last advert report. This is what the liveness
   *  check measures, so read it before you believe a recovery was real. */
  uint32_t silenceMs() const {
    if (!_ever_heard) return 0;
    long since = (long)(millis() - _last_report_ms);
    return since > 0 ? (uint32_t)since : 0;    // can be briefly negative; see loop()
  }
  /** Times the SoftDevice refused to start the connectable advert. */
  uint32_t numAdvFailures() const { return _num_adv_fail; }
  uint32_t numPresenceAdverts() const { return _num_presence; }
  /** The last SoftDevice error from the presence beacon. 0 means none. */
  uint32_t presenceError() const { return _presence_err; }
  /** Mean RSSI over all reports, as a measure of ambient BLE traffic. There is
   *  no way to sample a true noise floor while the SoftDevice scans. */
  int8_t meanReportRssi() const {
    return _report_count ? (int8_t)(_rssi_sum / (int32_t)_report_count) : 0;
  }

private:
  /* Advert interval, in 625us units. 244 is 152.5ms, which is the interval the
     connectable advert already used. */
  static const uint16_t ADV_INTERVAL = 244;

  /* Scan interval and window, in 625us units. 100ms and 50ms, so the scanner
     listens half of the time. A full duty cycle is deliberately not used: the
     SoftDevice needs slack for our own advert and for every connection, and a
     scan is what dominates BLE current at about 4.6mA. A beacon repeats every
     152.5ms, so half duty finds a new peer in well under a second. */
  static const uint16_t SCAN_INTERVAL = 160;
  static const uint16_t SCAN_WINDOW = 80;

  /* How often the presence beacon may be rebuilt. The beacon itself runs with
     no duration limit, so it repeats every ADV_INTERVAL until it is stopped.
     This paces the rebuild, which happens only when the battery reading moves
     or the SoftDevice refused the last attempt. */
  static const uint32_t PRESENCE_INTERVAL_MS = 3000;

  /* Retries must be PACED. loop() runs about 20,000 times a second, so an
     unpaced retry sends twenty thousand scan_stop and scan_start pairs each
     second and starves the stack. Reports then collapse to almost nothing, the
     peer link drops, and the node stops to advertise. A restart that the stack
     refuses needs time before it can succeed, not immediate repetition. */
  static const uint32_t SCAN_RETRY_MS = 1000;
  /* The advert has the same hazard, because loop() asserts it every pass. Pace
     the FAILURES only, so a success leaves the next pass free to assert again. */
  static const uint32_t ADV_RETRY_MS = 1000;

  /* Report flow is the ONLY observable truth about the scanner. No call asks
     the SoftDevice whether it scans, and we are not the only owner:
     BLECentral::connect() passes Bluefruit.Scanner.getParams(), and Bluefruit's
     own BLEScanner keeps its own buffer, its own flag and its own restart on
     disconnect. Any belief that we hold about the scan state can become false
     without notice. This check thus watches whether reports arrive.

     A scanner in a populated area hears adverts constantly: 126 a second,
     measured here. Thirty seconds of silence is thus about a thousand missing
     reports, which is conclusive and not merely suggestive. */
  static const uint32_t SILENCE_LIMIT_MS = 30000;
  /* ...but only for a node that has shown it lives somewhere busy. A repeater
     that is alone in the spectrum can be silent for half an hour with no fault,
     and must never restart its radio for that. One report a second, averaged
     since boot, is far below what a populated area gives and far above what an
     empty one gives. */
  static const uint32_t SILENCE_MIN_RATE_HZ = 1;

  bool startConnectableAdvert();
  bool presenceAdvert();
  bool armScan();
  void onAdvReport(const ble_gap_evt_adv_report_t* report);

  static void scan_cb(ble_gap_evt_adv_report_t* report);

  bool _running = false;
  uint16_t _company_id = 0;
  uint16_t _group_marker = 0;
  peer_handler_t _handler = nullptr;

  uint8_t _adv_handle = 0;
  uint8_t _batt_dv = 0;
  uint8_t _adv_batt_dv = 0;         // the value in the connectable advert now
  uint8_t _presence_batt_dv = 0;    // the value in the presence beacon now
  bool _adv_data_built = false;
  bool _presence_running = false;
  unsigned long _next_presence_ms = 0;
  unsigned long _next_adv_try_ms = 0;
  uint32_t _presence_err = 0;

  /* Our INTENT: the scanner must run. There is no SoftDevice call that asks
     whether it does, so this is the only model available. Everything that stops
     a scan clears the flag, and loop() drives it back to true until the stack
     accepts. One flag, one authority, retried and not assumed. */
  bool _scan_armed = false;
  unsigned long _next_scan_try_ms = 0;

  /* The SoftDevice keeps a pointer to the advert buffer while the set is
     configured. The buffer must thus outlive each advert, and nothing may touch
     it while an advert is on the air. */
  uint8_t _adv_buf[31];
  uint8_t _adv_len = 0;

  /* volatile: the Bluefruit callback task writes these and the main loop reads
     them. Without volatile the compiler can hold the read across loop passes
     and compare against a value that never changes. That is what happened
     before: the liveness check fired twice a minute while silenceMs(), a
     separate call, correctly reported tens of milliseconds. */
  volatile unsigned long _last_report_ms = 0;
  volatile bool _ever_heard = false;
  volatile uint32_t _report_count = 0;
  uint32_t _num_recoveries = 0;
  uint32_t _num_adv_fail = 0;
  uint32_t _num_presence = 0;
  uint32_t _num_beacons = 0;
  uint32_t _num_foreign_beacons = 0;
  uint32_t _report_cpu_us = 0;
  int32_t  _rssi_sum = 0;
};

#if BLE_DISCOVERY_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define BLE_DISC_DEBUG_PRINTLN(F, ...) Serial.printf("BleDiscovery: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_DISC_DEBUG_PRINTLN(...) {}
#endif
