#include "BleBroadcast.h"

#include <Arduino.h>
#include <string.h>
#include "ble_gap.h"

/* Exactly one instance is possible: the SoftDevice has one advertising set and
   one scanner, and the event callback is a free function. Same singleton shape
   SerialBLEInterface uses next door. */
static BleBroadcast* instance = nullptr;

/* The SoftDevice keeps this buffer for the lifetime of the scan and hands it
   back one report at a time, so it must outlive every call. 255 is both the
   extended-scan minimum and our own maximum advert, so one of our datagrams is
   never delivered truncated. */
static uint8_t s_scan_buf[BLE_GAP_SCAN_BUFFER_EXTENDED_MIN];
static ble_data_t s_scan_data;

/* Set when a scan re-arm fails in event context (the SoftDevice can transiently
   report NRF_ERROR_RESOURCES while other roles hold slots). loop() retries;
   without this the node would go permanently deaf on a single transient. */
static volatile bool s_scan_needs_rearm = false;

bool BleBroadcast::initStack(const char* name) {
  static bool done = false;
  if (done) return true;

  /* Only bring the stack up if nobody already has. A repeater build with a
     companion or CLI interface has one; a bare one does not. */
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (!sd_enabled) {
    if (!Bluefruit.begin()) {
      BLE_BCAST_DEBUG_PRINTLN("Bluefruit.begin() failed");
      return false;
    }
  }

  if (name != nullptr) Bluefruit.setName(name);
  done = true;
  return true;
}

bool BleBroadcast::begin(uint16_t company_id, rx_handler_t handler, event_chain_t chain) {
  if (_running) return true;

  instance = this;
  _company_id = company_id;
  _handler = handler;
  _chain = chain;

  /* Take the raw event slot and forward what we do not consume. This must come
     after every other subsystem's begin(), or ours is the one that gets
     overwritten and we never see an advert report. */
  Bluefruit.setEventCallback(onBLEEvent);

  /* Prove we can drive the advertising set before claiming to be running.
     BLE_GAP_ADV_SET_COUNT_MAX is 1, and Bluefruit already owns that set with no
     accessor for its handle -- but it consistently allocates handle 0, which
     SerialBLEInterface::isAdvertising() also assumes when it calls
     sd_ble_gap_adv_addr_get(0, ...). Rather than trust that silently, configure
     it once here and refuse to start if the assumption is wrong. */
  _adv_handle = 0;
  bool was_advertising = Bluefruit.Advertising.isRunning();
  if (was_advertising) Bluefruit.Advertising.stop();

  uint8_t probe[8] = { 0 };
  uint32_t err = configureAdvSet(probe, sizeof(probe));

  /* Hand the set straight back, whatever happened. Bluefruit's start()
     reconfigures it with its own connectable data, so nothing of ours lingers. */
  if (was_advertising) Bluefruit.Advertising.start(0);

  if (err != NRF_SUCCESS) {
    BLE_BCAST_DEBUG_PRINTLN("adv set 0 unavailable (err 0x%08lX) -- not starting", (unsigned long)err);
    Bluefruit.setEventCallback(_chain);   // give the slot back
    instance = nullptr;
    return false;
  }

  _queue_len = 0;
  _bursting = false;
  _restore_connectable = false;
  _window_started_ms = millis();
  _burst_ms_in_window = 0;
  _next_burst_allowed_ms = 0;

  if (!armScan(true)) {
    BLE_BCAST_DEBUG_PRINTLN("scan_start failed -- not starting");
    Bluefruit.setEventCallback(_chain);
    instance = nullptr;
    return false;
  }

  _running = true;
  BLE_BCAST_DEBUG_PRINTLN("running, company=0x%04X, max payload %u", _company_id, (unsigned)MAX_PAYLOAD);
  return true;
}

void BleBroadcast::end() {
  if (!_running) return;

  sd_ble_gap_scan_stop();
  if (_bursting) {
    sd_ble_gap_adv_stop(_adv_handle);
    finishBurst();
  }
  _queue_len = 0;
  _running = false;

  Bluefruit.setEventCallback(_chain);
  instance = nullptr;
  BLE_BCAST_DEBUG_PRINTLN("stopped");
}

uint32_t BleBroadcast::configureAdvSet(const uint8_t* payload, uint8_t len) {
  /* One Manufacturer Specific Data AD structure:
       [length][0xFF][company lo][company hi][payload...]
     The length byte counts everything after itself. */
  _adv_buf[0] = (uint8_t)(3 + len);
  _adv_buf[1] = 0xFF;
  _adv_buf[2] = (uint8_t)(_company_id & 0xFF);
  _adv_buf[3] = (uint8_t)(_company_id >> 8);
  memcpy(&_adv_buf[4], payload, len);
  _adv_len = (uint8_t)(AD_OVERHEAD + len);

  ble_gap_adv_data_t adv_data;
  memset(&adv_data, 0, sizeof(adv_data));
  adv_data.adv_data.p_data = _adv_buf;
  adv_data.adv_data.len = _adv_len;
  adv_data.scan_rsp_data.p_data = NULL;
  adv_data.scan_rsp_data.len = 0;

  ble_gap_adv_params_t adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  /* Non-connectable and non-scannable: nobody may connect to a bridge advert,
     and no scan requests means no scan-response round trips wasting air time. */
  adv_params.properties.type = BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
  adv_params.p_peer_addr = NULL;
  adv_params.interval = ADV_INTERVAL;
  adv_params.duration = 0;                 // bounded by max_adv_evts instead
  adv_params.max_adv_evts = MAX_ADV_EVTS;
  adv_params.filter_policy = BLE_GAP_ADV_FP_ANY;
  adv_params.primary_phy = BLE_GAP_PHY_1MBPS;
  adv_params.secondary_phy = BLE_GAP_PHY_1MBPS;

  return sd_ble_gap_adv_set_configure(&_adv_handle, &adv_data, &adv_params);
}

bool BleBroadcast::send(const uint8_t* payload, uint8_t len) {
  if (!_running || len == 0 || len > MAX_PAYLOAD) {
    _num_tx_dropped++;
    return false;
  }
  if (_queue_len >= QUEUE_SIZE) {
    _num_tx_dropped++;
    BLE_BCAST_DEBUG_PRINTLN("tx queue full, dropping %u bytes", (unsigned)len);
    return false;
  }

  Datagram& d = _queue[_queue_len++];
  d.len = len;
  memcpy(d.buf, payload, len);
  return true;
}

void BleBroadcast::shiftQueueLeft() {
  if (_queue_len == 0) return;
  _queue_len--;
  for (uint8_t i = 0; i < _queue_len; i++) {
    _queue[i] = _queue[i + 1];
  }
}

bool BleBroadcast::startBurst(const Datagram& d) {
  /* Only take the advertising set away from the connectable advert if it
     actually had it -- while a client is connected Bluefruit has already
     stopped advertising, and restoring it afterwards would let a second client
     in behind SerialBLEInterface's single _conn_handle. */
  _restore_connectable = Bluefruit.Advertising.isRunning();
  if (_restore_connectable) _shares_adv_set = true;
  if (_restore_connectable) Bluefruit.Advertising.stop();

  uint32_t err = configureAdvSet(d.buf, d.len);
  if (err == NRF_SUCCESS) {
    sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, _adv_handle, Bluefruit.getTxPower());
    err = sd_ble_gap_adv_start(_adv_handle, BLE_CONN_CFG_TAG_DEFAULT);
  }

  if (err != NRF_SUCCESS) {
    BLE_BCAST_DEBUG_PRINTLN("burst failed: 0x%08lX", (unsigned long)err);
    _num_tx_dropped++;
    finishBurst();
    return false;
  }

  _bursting = true;
  _burst_started_ms = millis();
  _num_sent++;
  return true;
}

void BleBroadcast::finishBurst() {
  unsigned long now = millis();
  if (_bursting) {
    _burst_ms_in_window += (uint32_t)(now - _burst_started_ms);
  }
  _bursting = false;
  _next_burst_allowed_ms = now + BURST_GAP_MS;

  /* Give the connectable CLI/DFU advert its set back. Guard on the connection
     count as well: a connection formed during the burst would make restarting
     advertising the wrong move. */
  if (_restore_connectable && Bluefruit.Periph.connected() == 0) {
    Bluefruit.Advertising.start(0);
  }
  _restore_connectable = false;
}

void BleBroadcast::loop() {
  if (!_running) return;

  unsigned long now = millis();

  if (s_scan_needs_rearm) {
    s_scan_needs_rearm = false;
    if (!armScan(false)) s_scan_needs_rearm = true;   // try again next loop
  }

  if (_bursting) {
    /* ADV_SET_TERMINATED should end every burst. If it somehow does not, we
       would hold the only advertising set forever and the diagnostic port
       would never be discoverable again. */
    if (now - _burst_started_ms > BURST_TIMEOUT_MS) {
      BLE_BCAST_DEBUG_PRINTLN("burst timed out, forcing stop");
      sd_ble_gap_adv_stop(_adv_handle);
      finishBurst();
    }
    return;
  }

  if (_queue_len == 0) return;
  if ((long)(now - _next_burst_allowed_ms) < 0) return;

  /* Roll the accounting window, then enforce the connectable-advert guarantee:
     a CLI/DFU port sharing the set must stay discoverable even while the bridge
     is busy. Skipped entirely when nothing else advertises -- throttling
     ourselves on behalf of nobody would just add latency. */
  if (now - _window_started_ms >= CONNECTABLE_PERIOD_MS) {
    _window_started_ms = now;
    _burst_ms_in_window = 0;
  }
  if (_shares_adv_set && _burst_ms_in_window >= CONNECTABLE_PERIOD_MS - CONNECTABLE_MIN_MS) {
    return;   // yield the rest of this window to connectable advertising
  }

  if (startBurst(_queue[0])) {
    shiftQueueLeft();
  } else {
    shiftQueueLeft();   // do not retry a datagram the SoftDevice refused
  }
}

bool BleBroadcast::armScan(bool first) {
  uint32_t err;
  if (first) {
    s_scan_data.p_data = s_scan_buf;
    s_scan_data.len = sizeof(s_scan_buf);

    ble_gap_scan_params_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.extended = 1;                 // the whole point: 255-byte adverts
    sp.active = 0;                   // passive; we never want scan responses
    sp.interval = SCAN_INTERVAL;
    sp.window = SCAN_WINDOW;
    sp.timeout = BLE_GAP_SCAN_TIMEOUT_UNLIMITED;
    sp.scan_phys = BLE_GAP_PHY_1MBPS;
    sp.filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL;

    err = sd_ble_gap_scan_start(&sp, &s_scan_data);
  } else {
    /* Continuing an existing scan: params MUST be NULL, buffer must be given
       back. Scanning is paused from the moment a report is delivered until
       this call, so any delay here is time spent deaf. */
    err = sd_ble_gap_scan_start(NULL, &s_scan_data);
  }

  if (err != NRF_SUCCESS) {
    BLE_BCAST_DEBUG_PRINTLN("scan_start(first=%d) failed: 0x%08lX", (int)first, (unsigned long)err);
    return false;
  }
  return true;
}

void BleBroadcast::onAdvReport(const ble_gap_evt_adv_report_t* report) {
  /* Only whole datagrams. Our adverts fit the buffer exactly, so anything
     truncated or still arriving is somebody else's traffic. */
  if (report->type.status != BLE_GAP_ADV_DATA_STATUS_COMPLETE) return;

  const uint8_t* p = report->data.p_data;
  uint16_t remaining = report->data.len;
  if (p == NULL) return;

  /* Walk the AD structures looking for our manufacturer data. Most reports in
     any populated area are other people's beacons and bail out here, well
     before the caller does anything expensive. */
  while (remaining >= 2) {
    uint8_t field_len = p[0];
    if (field_len == 0 || field_len + 1 > remaining) break;   // malformed

    uint8_t type = p[1];
    if (type == 0xFF && field_len >= 3) {
      uint16_t company = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
      if (company == _company_id) {
        const uint8_t* payload = &p[4];
        uint8_t payload_len = (uint8_t)(field_len - 3);
        _num_recv++;
        if (_handler) {
          _handler(payload, payload_len, report->peer_addr.addr, report->rssi);
        }
        return;
      }
    }

    p += field_len + 1;
    remaining -= field_len + 1;
  }
}

void BleBroadcast::onBLEEvent(ble_evt_t* evt) {
  BleBroadcast* self = instance;

  if (self != nullptr && self->_running) {
    switch (evt->header.evt_id) {
      case BLE_GAP_EVT_ADV_REPORT: {
        self->onAdvReport(&evt->evt.gap_evt.params.adv_report);
        /* The SoftDevice pauses scanning on every completed report and hands
           our buffer back; without this call the node simply stops hearing. */
        if (evt->evt.gap_evt.params.adv_report.type.status
              != BLE_GAP_ADV_DATA_STATUS_INCOMPLETE_MORE_DATA) {
          if (!self->armScan(false)) s_scan_needs_rearm = true;
        }
        break;
      }

      case BLE_GAP_EVT_ADV_SET_TERMINATED: {
        /* Bluefruit's own connectable advert raises this too when it times out,
           so only claim it if the burst was ours. */
        if (self->_bursting) {
          self->finishBurst();
          return;   // consumed
        }
        break;
      }

      default:
        break;
    }
  }

  /* Everything else belongs to whoever held the slot before us -- notably
     SerialBLEInterface, which needs CONN_PARAM_UPDATE_REQUEST. */
  if (self != nullptr && self->_chain) {
    self->_chain(evt);
  }
}
