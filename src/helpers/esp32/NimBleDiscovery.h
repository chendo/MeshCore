#pragma once

#include <stdint.h>
#include <stddef.h>

#include <Arduino.h>
#include <NimBLEDevice.h>

/* The beacon record and the group marker are part of the bridge's wire format,
   so they live beside the frame codec. That header needs no BLE stack and no
   Arduino, which is how the host tests reach them. */
#include "helpers/bridges/BleBridgeFrame.h"
#include "helpers/bridges/BleLinkTypes.h"
#include "NimBleStack.h"

/**
 * @brief  How two bridge nodes find each other over BLE on ESP32.
 *
 * The counterpart to BleDiscovery, and the SAME beacon on the air: one legacy
 * advert, one Manufacturer Specific Data structure, company 0xFFFF, record
 * version, two-byte group marker, one byte of battery in decivolts, at an
 * interval of 152.5ms. A ThinkNode M1 and a ThinkNode M5 see each other because
 * every one of those numbers matches.
 *
 * The advert arbiter is the same too, and for the same reason:
 *
 *  - a CONNECTABLE advert while a peripheral slot is free, so a peer can dial
 *    us and a person can reach the node;
 *  - a NON-CONNECTABLE presence beacon while no slot is free, because a
 *    connectable advert needs a free connection slot and a node that stops to
 *    advertise while it bridges perfectly well is a node nobody can reach.
 *
 * EXACTLY ONE instance can exist. NimBLE has one legacy advertiser and one
 * scanner, and this class must be their only owner: NimBLEServer's
 * advertise-on-disconnect is turned off in the link backend for that reason. Do
 * not compile a BLE CLI or companion interface into a build that carries this
 * bridge. On ESP32 that is not merely a rule about ownership -- NimBLE and
 * Bluedroid cannot be present in one binary at all.
 */
class NimBleDiscovery : public NimBLEScanCallbacks {
public:
  /** Called for each beacon that carries our company ID and our group marker.
   *  Runs on the NimBLE host task. Keep it short. */
  typedef void (*peer_handler_t)(const BleAddr& addr, int8_t rssi);

  /** company(2) + version(1) + marker(2) + battery decivolts(1). */
  static const uint8_t BEACON_LEN = BleBridgeFrame::BEACON_LEN;

  NimBleDiscovery() {}

  /**
   * @brief  Start the BLE stack with the roles that a bridge needs.
   *
   * Static, and on the discovery class, because the stack must be up before
   * any layer of the bridge does anything, and discovery is the layer that
   * owns the advertiser. BleDiscovery answers the same two calls, so BLEBridge
   * needs no platform test of its own.
   */
  static bool stackBegin(const char* name, uint8_t prph, uint8_t central) {
    return NimBleStack::ensure(name, prph, central);
  }

  /** Our own BLE address, for the tie-break that decides which end dials. */
  static bool selfAddr(BleAddr& out);

  /**
   * @param company_id     the Manufacturer ID that tags our beacon.
   * @param group_marker   2 bytes derived from the group secret.
   * @param handler        called for each peer beacon that matches.
   * @returns false if NimBLE refused. Nothing runs in that case.
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
   * A connect attempt stops the scanner: NimBLE cannot scan and open a
   * connection at the same moment, so NimBLEClient::connect() stops the scan
   * and nothing starts it again. A node that dials a peer would go deaf from
   * that moment on. This is the same hazard the SoftDevice has, and it has the
   * same remedy.
   */
  void requestRescan() { _scan_armed = false; }

  /** Battery voltage for the beacon. Safe to call every loop pass. The advert
   *  is rebuilt only when the decivolt value changes. */
  void setBattery(uint16_t batt_mv) { _batt_dv = (uint8_t)(batt_mv / 100); }

  /**
   * @brief  CPU spent on advert reports, in microseconds.
   *
   * This is the only part of the scan that we control. It does NOT include
   * radio time: the controller still receives and decodes every advert on the
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
    return since > 0 ? (uint32_t)since : 0;
  }
  /** Times NimBLE refused to start the connectable advert. */
  uint32_t numAdvFailures() const { return _num_adv_fail; }
  uint32_t numPresenceAdverts() const { return _num_presence; }
  /** 0 when the presence beacon last started cleanly, 1 when it did not.
   *  NimBLE reports a bool where the SoftDevice reports a code, so this side
   *  cannot say more than whether it worked. */
  uint32_t presenceError() const { return _presence_err; }
  /** Mean RSSI over all reports, as a measure of ambient BLE traffic. */
  int8_t meanReportRssi() const {
    return _report_count ? (int8_t)(_rssi_sum / (int32_t)_report_count) : 0;
  }

private:
  /* Advert interval, in 625us units, as NimBLE takes it. 244 is 152.5ms, which
     is what the nRF52 side uses. */
  static const uint16_t ADV_INTERVAL = 244;

  /* Scan interval and window, in MILLISECONDS: NimBLE 2.x converts to 625us
     units itself, where the SoftDevice takes those units directly. 100ms and
     50ms, so the scanner listens half of the time -- the same duty cycle as the
     nRF52 side, and for the same reasons. A full duty cycle is deliberately not
     used: the controller needs slack for our own advert and for every
     connection, and a scan is what dominates BLE current. */
  static const uint16_t SCAN_INTERVAL_MS = 100;
  static const uint16_t SCAN_WINDOW_MS = 50;

  static const uint32_t PRESENCE_INTERVAL_MS = 3000;

  /* Retries must be PACED. loop() runs thousands of times a second, so an
     unpaced retry would send thousands of stop and start pairs each second and
     starve the stack. */
  static const uint32_t SCAN_RETRY_MS = 1000;
  static const uint32_t ADV_RETRY_MS = 1000;

  /* Report flow is the only observable truth about the scanner. A scanner in a
     populated area hears adverts constantly, so thirty seconds of silence after
     it heard traffic is conclusive. */
  static const uint32_t SILENCE_LIMIT_MS = 30000;
  /* ...but only for a node that has shown it lives somewhere busy. A repeater
     alone in the spectrum can be silent for half an hour with no fault. */
  static const uint32_t SILENCE_MIN_RATE_HZ = 1;

  /* A legacy advert carries 31 bytes in TOTAL: 3 for the flags and 8 for the
     beacon block, which leaves 20 for the name structure -- 2 of header and 18
     of text. An overrun is not a soft failure, it makes the whole advert
     invalid, and the node then stays invisible. */
  static const uint8_t NAME_BUDGET = 31 - 3 - 8 - 2;

  bool startAdvert(bool connectable);
  bool armScan();
  void onAdvReport(const NimBLEAdvertisedDevice* dev);

  /* NimBLEScanCallbacks. onResult fires for every advert, because begin() asks
     for duplicates: a beacon that is heard once and then filtered out would
     make a peer undiscoverable after a link drops, and would make the liveness
     check below read a healthy scanner as a dead one. */
  void onResult(const NimBLEAdvertisedDevice* dev) override;

  bool _running = false;
  uint16_t _company_id = 0;
  uint16_t _group_marker = 0;
  peer_handler_t _handler = nullptr;

  uint8_t _batt_dv = 0;
  uint8_t _adv_batt_dv = 0;         // the value in the advert now
  bool _adv_connectable = true;     // which advert is on the air
  bool _adv_data_built = false;
  unsigned long _next_presence_ms = 0;
  unsigned long _next_adv_try_ms = 0;
  uint32_t _presence_err = 0;

  /* Our INTENT: the scanner must run. Everything that stops a scan clears this
     flag, and loop() drives it back to true until the stack accepts. */
  bool _scan_armed = false;
  unsigned long _next_scan_try_ms = 0;

  /* volatile: the NimBLE host task writes these and the main loop reads them.
     Without volatile the compiler can hold the read across loop passes and
     compare against a value that never changes. */
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

#if BLE_DISCOVERY_DEBUG_LOGGING
  #define BLE_DISC_DEBUG_PRINTLN(F, ...) Serial.printf("NimBleDiscovery: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_DISC_DEBUG_PRINTLN(...) {}
#endif
