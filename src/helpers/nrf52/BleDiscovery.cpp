#include "BleDiscovery.h"
#include "BleStack.h"
#ifdef LOOP_WATCHDOG_MS
  #include "LoopWatchdog.h"
#endif

#include <Arduino.h>
#include <string.h>
#include "ble_gap.h"

/* Exactly one instance is possible. The SoftDevice has one advertising set and
   one scanner, and the scan callback is a free function. SerialBLEInterface
   next door has the same singleton shape. */
static BleDiscovery* instance = nullptr;

bool BleDiscovery::begin(uint16_t company_id, uint16_t group_marker,
                         peer_handler_t handler) {
  if (_running) return true;

  instance = this;
  _company_id = company_id;
  _group_marker = group_marker;
  _handler = handler;
  _adv_handle = 0;
  _adv_data_built = false;

  /* Become the ONLY owner of the advertising set.

     Bluefruit's BLEAdvertising believes it owns set 0 and keeps the state in
     its own flag. The presence beacon below drives that same set directly, and
     a raw sd_ble_gap_adv_start is invisible to Advertising.isRunning(). With
     the automatic restart on, Bluefruit's disconnect handler calls start() on a
     set that we still hold, and afterwards neither owner tries again. The node
     then stays unreachable while it bridges perfectly well. That fault took out
     both repeaters, each time just after a CLI session closed.

     With the automatic restart off there is one authority: the arbiter in
     loop(), which asserts the advert on every pass. */
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL, ADV_INTERVAL);
  /* No fast-to-slow change. A duration makes the SoftDevice raise
     ADV_SET_TERMINATED, and Bluefruit answers that event by starting its own
     advert on set 0. While the presence beacon holds that set, such a restart
     stomps the beacon. Both adverts thus run with no duration limit, and
     nothing raises ADV_SET_TERMINATED at all. */
  Bluefruit.Advertising.setFastTimeout(0);

  if (!startConnectableAdvert()) {
    /* The set is not ours to drive, so nothing here can work. Say so now
       instead of a silent failure that looks like an empty neighbourhood. */
    BLE_DISC_DEBUG_PRINTLN("connectable advert refused; not starting");
    instance = nullptr;
    return false;
  }

  Bluefruit.Scanner.setRxCallback(scan_cb);
  Bluefruit.Scanner.useActiveScan(false);      // passive; we never want a scan response
  /* Bluefruit must not restart the scanner by itself. Two owners of the scan
     state is the fault that this class exists to avoid. loop() is the one
     authority, and it restarts the scanner within a second of any stop. */
  Bluefruit.Scanner.restartOnDisconnect(false);

  if (!armScan()) {
    BLE_DISC_DEBUG_PRINTLN("scan_start failed; not starting");
    instance = nullptr;
    return false;
  }

  _running = true;
  BLE_DISC_DEBUG_PRINTLN("running, company=0x%04X marker=0x%04X",
                         _company_id, _group_marker);
  return true;
}

void BleDiscovery::end() {
  if (!_running) return;

  Bluefruit.Scanner.stop();
  _scan_armed = false;
  if (_presence_running) {
    sd_ble_gap_adv_stop(_adv_handle);
    _presence_running = false;
  }
  Bluefruit.Advertising.stop();
  _running = false;
  instance = nullptr;
  BLE_DISC_DEBUG_PRINTLN("stopped");
}

bool BleDiscovery::startConnectableAdvert() {
  /* Rebuild only when the battery reading moved. Decivolts change slowly, so
     this costs one stop and start every few minutes at most. A rebuild on every
     pass would restart the advert thousands of times a second. */
  if (!_adv_data_built || _adv_batt_dv != _batt_dv) {
    uint8_t rec[BEACON_LEN];
    BleBridgeFrame::buildBeaconRecord(rec, _company_id, _group_marker, _batt_dv);
    if (Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addManufacturerData(rec, BEACON_LEN);
    /* addName() truncates to what is left of the 31 bytes, so a long node name
       cannot make the SoftDevice reject the whole advert. */
    Bluefruit.Advertising.addName();
    _adv_batt_dv = _batt_dv;
    _adv_data_built = true;
  }
  if (Bluefruit.Advertising.isRunning()) return true;
  return Bluefruit.Advertising.start(0);
}

/* A NON-CONNECTABLE beacon, for the times when the connectable advert cannot
   run. It carries the same beacon record plus the node name, so a scanner sees
   the same node whichever advert it caught. */
bool BleDiscovery::presenceAdvert() {
  char nm[32];
  uint8_t nlen = Bluefruit.getName(nm, sizeof(nm));

  /* A legacy advert carries 31 bytes in TOTAL. The flags cost 3 and the beacon
     block costs 8, which leaves 20 for the name AD structure: 2 of header and
     18 of text. An overrun is not a soft failure. sd_ble_gap_adv_set_configure
     rejects the whole advert, and the node then stays invisible, which is the
     exact fault that this beacon prevents. The name of the production node is
     19 characters, so this truncation does real work. */
  const uint8_t NAME_BUDGET = 31 - 3 /*flags*/ - 8 /*beacon block*/ - 2 /*name hdr*/;
  if (nlen > NAME_BUDGET) nlen = NAME_BUDGET;

  uint8_t rec[BEACON_LEN];
  BleBridgeFrame::buildBeaconRecord(rec, _company_id, _group_marker, _batt_dv);

  uint8_t i = 0;
  _adv_buf[i++] = 2; _adv_buf[i++] = 0x01; _adv_buf[i++] = 0x04;   // Flags: no BR/EDR
  _adv_buf[i++] = (uint8_t)(BEACON_LEN + 1);
  _adv_buf[i++] = 0xFF;                                            // Manufacturer data
  memcpy(&_adv_buf[i], rec, BEACON_LEN); i += BEACON_LEN;
  _adv_buf[i++] = (uint8_t)(nlen + 1);
  _adv_buf[i++] = 0x09;                                            // Complete Local Name
  memcpy(&_adv_buf[i], nm, nlen); i += nlen;
  _adv_len = i;

  ble_gap_adv_data_t adv_data;
  memset(&adv_data, 0, sizeof(adv_data));
  adv_data.adv_data.p_data = _adv_buf;
  adv_data.adv_data.len = _adv_len;

  ble_gap_adv_params_t adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  /* Legacy, NON-connectable and NON-scannable (ADV_NONCONN_IND).
     Non-connectable is the point: this type needs no free connection slot.
     Non-scannable because ble_gap.h states that scan response data "can only be
     specified for a type that is scannable". The scannable type with no scan
     response is exactly the kind of request that the SoftDevice rejects. This
     type carries the full 31 bytes on its own and needs no second buffer. */
  adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
  adv_params.primary_phy = BLE_GAP_PHY_1MBPS;
  adv_params.secondary_phy = BLE_GAP_PHY_1MBPS;
  adv_params.interval = ADV_INTERVAL;
  /* No duration limit. A duration makes the SoftDevice raise
     ADV_SET_TERMINATED, and Bluefruit answers that event by starting its own
     connectable advert on the same set. The beacon thus runs until loop()
     stops it. */
  adv_params.duration = BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED;
  adv_params.filter_policy = BLE_GAP_ADV_FP_ANY;

  /* Record WHY a beacon failed. The first version returned a bare false, so a
     beacon that never once configured looked the same as a beacon that was
     merely not due. It took a hardware session to see that it had never run.
     An error that nobody can read is an error that somebody will assume away. */
  sd_ble_gap_adv_stop(_adv_handle);
  _presence_err = sd_ble_gap_adv_set_configure(&_adv_handle, &adv_data, &adv_params);
  if (_presence_err != NRF_SUCCESS) return false;
  /* conn_cfg_tag is documented as ignored for a non-connectable advert. Pass
     the default and not an arbitrary tag. */
  _presence_err = sd_ble_gap_adv_start(_adv_handle, BLE_CONN_CFG_TAG_DEFAULT);
  if (_presence_err != NRF_SUCCESS) return false;
  _presence_running = true;
  _presence_batt_dv = _batt_dv;
  return true;
}

bool BleDiscovery::armScan() {
  /* Stop first. The SoftDevice takes scan parameters on a fresh start only, and
     a stop on an idle scanner is a harmless no-op. One path thus covers every
     reason that the intent was cleared, instead of one remedy for each cause. */
  Bluefruit.Scanner.stop();
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
  if (!Bluefruit.Scanner.start(0)) {
    BLE_DISC_DEBUG_PRINTLN("scan_start failed");
    _scan_armed = false;
    return false;
  }
  _scan_armed = true;
  return true;
}

void BleDiscovery::loop() {
  if (!_running) return;

  /* Exactly one advert owns the set at a time. The connectable advert wins
     while a peripheral slot is free; the presence beacon takes over only when
     no slot is free. Without this split the two fight, because a raw
     sd_ble_gap_adv_start is invisible to Bluefruit.Advertising.isRunning(). */
  const bool slot_free = (Bluefruit.Periph.connected() < BleStack::periphSlots());

  if (slot_free && _presence_running) {
    sd_ble_gap_adv_stop(_adv_handle);      // give the set back to the connectable advert
    _presence_running = false;
  }

  /* Advertise whenever a peripheral slot is still FREE, and not merely when no
     slot is used. A peer link occupies one of them, so a test for zero
     connections made a node with a link stop to advertise. Its CLI and its DFU
     port then became unreachable at the exact moment that bridging began to
     work, which is the worst possible time to lose the way in.

     Checked on EVERY pass. An earlier version checked only after a busy period,
     which a quiet node reaches rarely or never. A node that lost its advert
     thus stayed invisible for as long as it ran, while it bridged perfectly
     well. That is not theoretical: it took out the production repeater, which
     still relayed LoRa with no way left to reach it over BLE. */
  if (slot_free && !Bluefruit.Advertising.isRunning()
      && (long)(millis() - _next_adv_try_ms) >= 0) {
    /* Count a refusal instead of an ignore. A silent spin here is what nobody
       finds out about. At about 17,000 loop passes a second a persistent
       refusal means 17,000 configure and start calls a second, which is the
       same shape as the scan spin that took BLE down completely. Pace the
       FAILURES only: a success leaves the next pass free to assert again, and a
       refusal backs off to once a second. */
    if (startConnectableAdvert()) {
      _next_adv_try_ms = millis();          // took: no delay before the next assert
    } else {
      _num_adv_fail++;
      _next_adv_try_ms = millis() + ADV_RETRY_MS;
    }
  }

  /* No slot is free, so the SoftDevice will refuse a connectable advert and the
     node is invisible to every scanner. This is how both repeaters came to be
     "missing" while they bridged perfectly. Fall back to the non-connectable
     beacon, which needs no connection slot. */
  if (!slot_free
      && (!_presence_running || _presence_batt_dv != _batt_dv)
      && (long)(millis() - _next_presence_ms) >= 0) {
    _next_presence_ms = millis() + PRESENCE_INTERVAL_MS;
    if (presenceAdvert()) _num_presence++;
  }

  /* Receive-path liveness. A scanner in a populated area hears adverts
     constantly, so long silence after it heard traffic means the scanner
     stopped and reported no error. The node still bridges outbound and looks
     healthy in its own telemetry. */
  uint32_t up_s = millis() / 1000;
  bool busy_environment = (up_s > 30)
      && (_report_count / (up_s ? up_s : 1) >= SILENCE_MIN_RATE_HZ);
  /* SIGNED difference. The callback task writes _last_report_ms, and this loop
     reads millis() before it reads that stamp, so the task can move the stamp
     in between and leave it just AHEAD of the captured millis(). Unsigned, that
     underflows to about four billion and passes any threshold. At 142 reports a
     second against 17,000 loop passes the window was hit about six times a
     minute, which is the spurious recovery rate that was observed. Signed, the
     same case is a small negative number. */
  if (_ever_heard && busy_environment
      && (long)(millis() - _last_report_ms) > (long)SILENCE_LIMIT_MS) {
    /* Deliberately NOT gated on having no connections. That gate looked prudent
       and did real harm: a peer link is a permanent central connection, so once
       bridging worked the check could never fire again, and a dead scanner
       became unrecoverable. Reports froze at 2879 while the link carried frames
       happily.

       A connection proves that the STACK is alive. It says nothing about the
       scanner, which is the only thing that this check is about. */
    _num_recoveries++;
    _last_report_ms = millis();          // one attempt per window, not per pass
    _scan_armed = false;
    _next_scan_try_ms = 0;
  }

  /* Bluefruit clears its own scan flag when a central connection opens, and it
     does not start the scanner again. Take that as evidence and re-arm. */
  if (_scan_armed && !Bluefruit.Scanner.isRunning()) _scan_armed = false;

  unsigned long now = millis();

  /* Drive the scanner back to our intent on every pass, until the stack
     accepts. The retry is the whole point: a connect attempt stops the scan AND
     leaves the SoftDevice briefly unable to start it, so the first attempt
     after we dial a peer is expected to fail. To discard that failure is what
     left the node that dialled permanently deaf. */
  if (!_scan_armed && (long)(now - _next_scan_try_ms) >= 0) {
    _next_scan_try_ms = now + SCAN_RETRY_MS;
    armScan();
  }
}

void BleDiscovery::onAdvReport(const ble_gap_evt_adv_report_t* report) {
  BleBridgeFrame::BeaconResult r =
      BleBridgeFrame::parseBeacon(report->data.p_data, report->data.len,
                                  _company_id, _group_marker);
  if (r == BleBridgeFrame::BEACON_NONE) return;      // somebody else's advert
  if (r == BleBridgeFrame::BEACON_FOREIGN) {
    /* A MeshCore bridge node with a different group secret, or another person's
       beacon on the shared development company ID. Counted, and not dialled:
       the marker is what stops a node from opening a link to every MeshCore
       device in radio range. */
    _num_foreign_beacons++;
    return;
  }
  _num_beacons++;
  if (_handler) _handler(report->peer_addr, report->rssi);
}

void BleDiscovery::scan_cb(ble_gap_evt_adv_report_t* report) {
  BleDiscovery* self = instance;
  if (self != nullptr && self->_running) {
#ifdef LOOP_WATCHDOG_MS
    /* This task preempts the main loop and runs constantly, which makes it a
       natural place to see that the loop has stopped. */
    LoopWatchdog::check();
#endif
    uint32_t t0 = micros();
    self->_report_count++;
    self->_last_report_ms = millis();
    self->_ever_heard = true;
    self->_rssi_sum += report->rssi;
    self->onAdvReport(report);
    self->_report_cpu_us += (uint32_t)(micros() - t0);
  }
  /* The SoftDevice pauses the scan on every report and holds our buffer until
     we hand it back. Without this call the node simply stops to hear. */
  Bluefruit.Scanner.resume();
}
