#include "BluefruitLinkBackend.h"

/* Vendor UUIDs for the bridge service. Random 128-bit, so nothing else claims
   them. The base is shared and only the 16-bit slot differs.

   These bytes are the WIRE FORMAT of the bridge. The ESP32 backend must present
   the same service and the same characteristic, or the two families cannot
   link. Bluefruit takes a 128-bit UUID in little-endian order, which is why the
   byte order here is the reverse of how the same UUID is written out on the
   ESP32 side. */
static const uint8_t BRIDGE_SVC_UUID[16] = {
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
  0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x7A, 0x40, 0x6E
};
static const uint8_t BRIDGE_CHR_UUID[16] = {
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
  0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x7A, 0x40, 0x6E
};

/* Named, and not temporaries: the compiler reads BLEService s(BLEUuid(x)) as a
   function declaration and not as an object. */
static BLEUuid s_svc_uuid(BRIDGE_SVC_UUID);
static BLEUuid s_chr_uuid(BRIDGE_CHR_UUID);

/* Peripheral side: what a peer that dials US talks to. */
static BLEService        s_svc(s_svc_uuid);
static BLECharacteristic s_chr(s_chr_uuid);

/* Central side: Bluefruit binds a client service to one connection at a time,
   so each outward link needs its own instance. A shared pair that we point at a
   different connection each time does not work. */
static BLEClientService        s_clt[BLE_LINK_MAX_LINKS] = {
  BLEClientService(s_svc_uuid), BLEClientService(s_svc_uuid), BLEClientService(s_svc_uuid)
};
static BLEClientCharacteristic s_cchr[BLE_LINK_MAX_LINKS] = {
  BLEClientCharacteristic(s_chr_uuid), BLEClientCharacteristic(s_chr_uuid),
  BLEClientCharacteristic(s_chr_uuid)
};

/* The GATT service and its characteristics are registered with the SoftDevice
   once for the life of the process. `set bridge.secret` calls restartBridge(),
   which ends and begins the bridge, so begin() runs more than once; a second
   s_svc.begin() would add a SECOND copy of the service to the attribute table
   and consume attribute RAM that never comes back. */
static bool s_gatt_ready = false;

static BluefruitLinkBackend::notify_cb_t  s_on_notify = nullptr;
static BluefruitLinkBackend::written_cb_t s_on_written = nullptr;

static void notify_trampoline(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
  if (s_on_notify == nullptr) return;
  for (uint8_t i = 0; i < BLE_LINK_MAX_LINKS; i++) {
    if (chr == &s_cchr[i]) { s_on_notify(i, data, len); return; }
  }
}

static void written_trampoline(uint16_t conn, BLECharacteristic* chr,
                               uint8_t* data, uint16_t len) {
  (void)chr;
  if (s_on_written) s_on_written(conn, data, len);
}

void BluefruitLinkBackend::begin(uint16_t chunk, conn_cb_t on_conn,
                                 disconn_cb_t on_disconn, notify_cb_t on_notify,
                                 written_cb_t on_written) {
  s_on_notify = on_notify;
  s_on_written = on_written;

  if (!s_gatt_ready) {
    s_gatt_ready = true;
    s_svc.begin();                     // must come before its characteristics
    s_chr.setProperties(CHR_PROPS_WRITE_WO_RESP | CHR_PROPS_NOTIFY);
    s_chr.setPermission(SECMODE_OPEN, SECMODE_OPEN);
    s_chr.setMaxLen(chunk);
    s_chr.setWriteCallback(written_trampoline);
    s_chr.begin();

    for (uint8_t i = 0; i < BLE_LINK_MAX_LINKS; i++) {
      s_clt[i].begin();
      s_cchr[i].setNotifyCallback(notify_trampoline);
      s_cchr[i].begin();
    }
  }

  Bluefruit.Central.setConnectCallback(on_conn);
  Bluefruit.Central.setDisconnectCallback(on_disconn);
}

bool BluefruitLinkBackend::discoverLink(uint8_t idx, uint16_t conn) {
  return s_clt[idx].discover(conn) && s_cchr[idx].discover() && s_cchr[idx].enableNotify();
}

uint16_t BluefruitLinkBackend::writeLink(uint8_t idx, const uint8_t* data, uint16_t len) {
  return s_cchr[idx].write(data, len);
}

bool BluefruitLinkBackend::notifyInbound(uint16_t conn, const uint8_t* data, uint16_t len) {
  return s_chr.notify(conn, data, len);
}
