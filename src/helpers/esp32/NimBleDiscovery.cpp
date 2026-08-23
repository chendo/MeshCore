#include "NimBleDiscovery.h"
#include "NimBleLinkBackend.h"

#include <Arduino.h>
#include <string.h>
#include <string>

/* Exactly one instance is possible. NimBLE has one legacy advertiser and one
   scanner, and this class owns both. */
static NimBleDiscovery* instance = nullptr;

bool NimBleDiscovery::selfAddr(BleAddr& out) {
  NimBLEAddress a = NimBLEDevice::getAddress();
  const uint8_t* v = a.getVal();
  if (v == nullptr) return false;
  memcpy(out.addr, v, 6);
  out.addr_type = a.getType();
  out.addr_id_peer = 0;
  return true;
}

bool NimBleDiscovery::begin(uint16_t company_id, uint16_t group_marker,
                            peer_handler_t handler) {
  if (_running) return true;

  instance = this;
  _company_id = company_id;
  _group_marker = group_marker;
  _handler = handler;
  _adv_data_built = false;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return false;
  adv->setAdvertisingInterval(ADV_INTERVAL);
  /* No scan response. The beacon fits in the 31 bytes of the advert itself, and
     a scannable advert with nothing to answer with is the kind of request a
     controller rejects. It also keeps the air quiet: a passive scanner never
     asks, so a scan response would be sent to nobody. */
  adv->enableScanResponse(false);

  if (!startAdvert(true)) {
    /* Say so now instead of a silent failure that looks like an empty
       neighbourhood. */
    BLE_DISC_DEBUG_PRINTLN("connectable advert refused; not starting");
    instance = nullptr;
    return false;
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan == nullptr) { instance = nullptr; return false; }
  scan->setActiveScan(false);         // passive; we never want a scan response
  /* Ask for duplicates. The controller's duplicate filter would report a peer
     once and then never again, which would leave a peer undiscoverable after a
     link dropped and would make the liveness check below read a working
     scanner as a dead one. */
  scan->setScanCallbacks(this, true);
  /* Store nothing. This scanner runs for the life of the node, so a results
     list would grow without bound. NimBLE erases each device once the callback
     has had it. */
  scan->setMaxResults(0);

  if (!armScan()) {
    BLE_DISC_DEBUG_PRINTLN("scan start failed; not starting");
    instance = nullptr;
    return false;
  }

  _running = true;
  BLE_DISC_DEBUG_PRINTLN("running, company=0x%04X marker=0x%04X",
                         _company_id, _group_marker);
  return true;
}

void NimBleDiscovery::end() {
  if (!_running) return;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan != nullptr) {
    scan->setScanCallbacks(nullptr);
    scan->stop();
  }
  _scan_armed = false;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv != nullptr) adv->stop();
  _running = false;
  instance = nullptr;
  BLE_DISC_DEBUG_PRINTLN("stopped");
}

/**
 * @brief  Put one advert on the air, connectable or not.
 *
 * Both carry the same beacon record and the same node name, so a scanner sees
 * the same node whichever one it caught. The connectable form is preferred, and
 * the non-connectable form is what keeps the node visible when no connection
 * slot is left for a connectable advert to use.
 */
bool NimBleDiscovery::startAdvert(bool connectable) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return false;

  /* Rebuild only when something in it moved. Decivolts change slowly, so this
     costs one stop and start every few minutes at most. A rebuild on every pass
     would restart the advert thousands of times a second. */
  if (!_adv_data_built || _adv_batt_dv != _batt_dv || _adv_connectable != connectable) {
    uint8_t rec[BEACON_LEN];
    BleBridgeFrame::buildBeaconRecord(rec, _company_id, _group_marker, _batt_dv);

    /* The node name, truncated to what is left of the 31 bytes. A long name
       must not be allowed to make the whole advert invalid, because the node is
       then invisible -- which is the exact fault this beacon exists to
       prevent. */
    std::string nm(NimBleStack::name());
    if (nm.size() > NAME_BUDGET) nm.resize(NAME_BUDGET);

    if (adv->isAdvertising()) adv->stop();
    adv->clearData();
    /* The same flags byte the nRF52 side sends: general discoverable and no
       BR/EDR for the connectable advert, and no BR/EDR alone for the beacon.
       The ESP32-S3 has no Bluetooth Classic at all, so the BR/EDR bit is the
       only honest value in both cases. */
    NimBLEAdvertisementData d;
    d.setFlags(connectable ? (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP)
                           : BLE_HS_ADV_F_BREDR_UNSUP);
    d.setManufacturerData(rec, BEACON_LEN);
    if (!nm.empty()) d.setName(nm);
    if (!adv->setAdvertisementData(d)) return false;

    /* Non-connectable AND non-scannable (ADV_NONCONN_IND) for the beacon: that
       type needs no free connection slot, which is the whole reason it exists.
       Non-scannable because there is no scan response to give. */
    adv->setConnectableMode(connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON);
    adv->setDiscoverableMode(connectable ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);

    _adv_batt_dv = _batt_dv;
    _adv_connectable = connectable;
    _adv_data_built = true;
  }

  if (adv->isAdvertising()) return true;
  /* No duration limit. A duration makes the controller end the advert, and
     nothing would start it again until the next arbiter pass. */
  return adv->start(0);
}

bool NimBleDiscovery::armScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan == nullptr) { _scan_armed = false; return false; }
  /* Stop first. Parameters are taken on a fresh start only, and a stop on an
     idle scanner is a harmless no-op. One path thus covers every reason the
     intent was cleared, instead of one remedy for each cause. */
  scan->stop();
  scan->setInterval(SCAN_INTERVAL_MS);
  scan->setWindow(SCAN_WINDOW_MS);
  /* Duration 0 is "until stopped". isContinue false clears anything held from
     a previous run; restart true lets NimBLE start it again after a scan that
     the controller ended by itself. */
  if (!scan->start(0, false, true)) {
    BLE_DISC_DEBUG_PRINTLN("scan start failed");
    _scan_armed = false;
    return false;
  }
  _scan_armed = true;
  return true;
}

void NimBleDiscovery::loop() {
  if (!_running) return;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return;

  /* Exactly one advert owns the advertiser at a time. The connectable one wins
     while a peripheral slot is free; the presence beacon takes over only when
     no slot is free. */
  const bool slot_free =
      NimBleLinkBackend::numPeripheralConns() < NimBleStack::periphSlots();

  /* Advertise whenever a peripheral slot is still FREE, and not merely when no
     slot is used. A peer link occupies one of them, so a test for zero
     connections would make a node with a link stop to advertise -- and lose the
     way in at the exact moment bridging began to work.

     Checked on EVERY pass, and the FAILURES paced: a success leaves the next
     pass free to assert again, and a refusal backs off to once a second. An
     unpaced refusal at thousands of passes a second is the same shape as a scan
     spin, which takes BLE down completely. */
  if (slot_free && (long)(millis() - _next_adv_try_ms) >= 0
      && (!adv->isAdvertising() || !_adv_connectable || _adv_batt_dv != _batt_dv)) {
    if (startAdvert(true)) {
      _next_adv_try_ms = millis();
    } else {
      _num_adv_fail++;
      _next_adv_try_ms = millis() + ADV_RETRY_MS;
    }
  }

  /* No slot is free, so a connectable advert has no connection to offer and the
     node would be invisible to every scanner. Fall back to the non-connectable
     beacon, which needs no connection slot. */
  if (!slot_free
      && (!adv->isAdvertising() || _adv_connectable || _adv_batt_dv != _batt_dv)
      && (long)(millis() - _next_presence_ms) >= 0) {
    _next_presence_ms = millis() + PRESENCE_INTERVAL_MS;
    if (startAdvert(false)) {
      _num_presence++;
      _presence_err = 0;
    } else {
      _presence_err = 1;
    }
  }

  /* Receive-path liveness. A scanner in a populated area hears adverts
     constantly, so long silence after it heard traffic means the scanner
     stopped and reported no error. The node still bridges outbound and looks
     healthy in its own telemetry. */
  uint32_t up_s = millis() / 1000;
  bool busy_environment = (up_s > 30)
      && (_report_count / (up_s ? up_s : 1) >= SILENCE_MIN_RATE_HZ);
  /* SIGNED difference. The host task writes _last_report_ms, and this loop
     reads millis() before it reads that stamp, so the task can move the stamp
     in between and leave it just AHEAD of the captured millis(). Unsigned, that
     underflows to about four billion and passes any threshold. */
  if (_ever_heard && busy_environment
      && (long)(millis() - _last_report_ms) > (long)SILENCE_LIMIT_MS) {
    /* Deliberately NOT gated on having no connections. A connection proves the
       STACK is alive; it says nothing about the scanner, which is the only
       thing this check is about. Gating it that way once made a dead scanner
       unrecoverable as soon as bridging worked. */
    _num_recoveries++;
    _last_report_ms = millis();          // one attempt per window, not per pass
    _scan_armed = false;
    _next_scan_try_ms = 0;
  }

  /* A connect attempt stops the scanner and NimBLE clears its own flag. Take
     that as evidence and re-arm. */
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (_scan_armed && scan != nullptr && !scan->isScanning()) _scan_armed = false;

  unsigned long now = millis();

  /* Drive the scanner back to our intent on every pass, until the stack
     accepts. The retry is the whole point: a connect attempt stops the scan AND
     leaves the controller briefly unable to start it, so the first attempt
     after we dial a peer is expected to fail. To discard that failure is what
     leaves the node that dialled permanently deaf. */
  if (!_scan_armed && (long)(now - _next_scan_try_ms) >= 0) {
    _next_scan_try_ms = now + SCAN_RETRY_MS;
    armScan();
  }
}

void NimBleDiscovery::onAdvReport(const NimBLEAdvertisedDevice* dev) {
  const std::vector<uint8_t>& ad = dev->getPayload();
  BleBridgeFrame::BeaconResult r =
      BleBridgeFrame::parseBeacon(ad.data(), ad.size(), _company_id, _group_marker);
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
  if (_handler) {
    BleAddr a;
    const NimBLEAddress& peer = dev->getAddress();
    memcpy(a.addr, peer.getVal(), 6);
    a.addr_type = peer.getType();
    a.addr_id_peer = 0;
    _handler(a, dev->getRSSI());
  }
}

void NimBleDiscovery::onResult(const NimBLEAdvertisedDevice* dev) {
  if (!_running || dev == nullptr) return;
  uint32_t t0 = micros();
  _report_count++;
  _last_report_ms = millis();
  _ever_heard = true;
  _rssi_sum += dev->getRSSI();
  onAdvReport(dev);
  _report_cpu_us += (uint32_t)(micros() - t0);
}
