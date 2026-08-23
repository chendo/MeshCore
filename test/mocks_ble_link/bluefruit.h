#pragma once

// A stand-in for the Adafruit Bluefruit nRF52 API. It is large enough to build
// and to run src/helpers/nrf52/BleLink.cpp on a host, and it holds only what
// that file uses. The same idea as test/mocks_radiolib: the real source file is
// what runs, so a test covers the shipped code and not a copy of it.
//
// A test drives the clock, the connections and the TX credits through the
// BleMock namespace at the end of this file.

#include <cstdint>
#include <cstring>
#include <vector>

static const uint16_t BLE_CONN_HANDLE_INVALID = 0xFFFF;
/* The real header sets 20. Only the count matters here, so keep it at the
   number of connections that this stand-in holds. */
static const uint16_t BLE_MAX_CONNECTION = 8;

#define CHR_PROPS_WRITE_WO_RESP 0x04
#define CHR_PROPS_NOTIFY        0x10
#define SECMODE_OPEN            0x11

typedef struct {
  uint8_t addr_id_peer : 1;
  uint8_t addr_type : 7;
  uint8_t addr[6];
} ble_gap_addr_t;

class BLECharacteristic;
class BLEClientCharacteristic;

namespace BleMock {

/* Outward links, and one more sink for the inbound peer. This must be at least
   BleLink::MAX_LINKS + 1, which this header cannot see. */
static const uint8_t MAX_SINKS = 4;
/* The sink that holds what went to the inbound peer. */
static const uint8_t INBOUND_SINK = 3;
static const uint8_t MAX_CONNS = 8;

struct Conn {
  bool used = false;
  bool connected = false;
  /* The role WE hold on this connection. A peer that dialled us makes us the
     peripheral; a link that we dialled makes us the central. BleLink sweeps
     the peripheral connections only. */
  bool periph = false;
  /* MITM pairing. The CLI needs it and a bridge peer never asks for it, so
     this is how BleLink keeps away from a CLI session. */
  bool secured = false;
  bool bonded = false;
  ble_gap_addr_t peer = {};
  uint16_t mtu = 247;
};

inline unsigned long now_ms = 0;
inline Conn conns[MAX_CONNS];

/* Bytes that the stack still accepts, over every link. -1 means no limit. A
   test sets this to model TX credits that run out part way through a frame. */
inline int credits = -1;
/* Everything that went out, in order, for each sink. */
inline std::vector<uint8_t> sink[MAX_SINKS];

/* The connection that the last notify() named. BLE_CONN_HANDLE_INVALID when
   nothing has been notified. */
inline uint16_t last_notify_conn = 0xFFFF;

/* GATT discovery on an outward link. A test sets this false to model a peer
   that connects but carries none of our service. */
inline bool discover_ok = true;
/* What Bluefruit.Central.connect() reports. */
inline bool connect_ok = true;
inline ble_gap_addr_t dial_addr = {};
inline uint32_t dials = 0;

/* The callbacks that BleLink registered. BleLink registers its GATT callbacks
   once for the life of the process, so reset() must NOT clear these. */
inline void (*connect_cb)(uint16_t) = nullptr;
inline void (*disconnect_cb)(uint16_t, uint8_t) = nullptr;
inline void (*notify_cb)(BLEClientCharacteristic*, uint8_t*, uint16_t) = nullptr;
inline void (*written_cb)(uint16_t, BLECharacteristic*, uint8_t*, uint16_t) = nullptr;

/* Client characteristics, in construction order, which is link order. */
inline BLEClientCharacteristic* client_chr[MAX_SINKS] = {nullptr, nullptr, nullptr, nullptr};
inline uint8_t client_chr_count = 0;

/** How many bytes of a write of len the stack accepts. */
inline uint16_t take(uint16_t len) {
  if (credits < 0) return len;
  uint16_t n = (uint16_t)(credits < (int)len ? credits : (int)len);
  credits -= n;
  return n;
}

}  // namespace BleMock

inline unsigned long millis() { return BleMock::now_ms; }

class BLEUuid {
public:
  BLEUuid() {}
  BLEUuid(const uint8_t uuid128[16]) { memcpy(_u, uuid128, 16); }
  uint8_t _u[16] = {0};
};

class BLEConnection {
public:
  uint16_t handle = BLE_CONN_HANDLE_INVALID;
  bool connected() const { return BleMock::conns[handle].connected; }
  void disconnect() { BleMock::conns[handle].connected = false; }
  bool secured() const { return BleMock::conns[handle].secured; }
  bool bonded() const { return BleMock::conns[handle].bonded; }
  ble_gap_addr_t getPeerAddr() const { return BleMock::conns[handle].peer; }
  uint16_t getMtu() const { return BleMock::conns[handle].mtu; }
};

namespace BleMock {
inline BLEConnection wrapper[MAX_CONNS];
}

class BLEService {
public:
  BLEService() {}
  explicit BLEService(const BLEUuid&) {}
  void begin() {}
};

class BLECharacteristic {
public:
  BLECharacteristic() {}
  explicit BLECharacteristic(const BLEUuid&) {}
  void setProperties(uint8_t) {}
  void setPermission(uint8_t, uint8_t) {}
  void setMaxLen(uint16_t) {}
  void setWriteCallback(void (*cb)(uint16_t, BLECharacteristic*, uint8_t*, uint16_t)) {
    BleMock::written_cb = cb;
  }
  void begin() {}
  /* The real notify() reports only a bool, so BleLink keeps every notify to one
     packet. Model that: a partial take is a refusal and nothing goes out.

     Named by connection, because BleLink notifies the peer it adopted and not
     every subscriber. A handle that is not live takes nothing, which is what
     the real one does. */
  bool notify(uint16_t conn_hdl, const void* data, uint16_t len) {
    if (conn_hdl >= BleMock::MAX_CONNS || !BleMock::conns[conn_hdl].connected) return false;
    if (BleMock::credits >= 0 && BleMock::credits < (int)len) return false;
    if (BleMock::credits >= 0) BleMock::credits -= len;
    BleMock::last_notify_conn = conn_hdl;
    const uint8_t* p = (const uint8_t*)data;
    std::vector<uint8_t>& s = BleMock::sink[BleMock::INBOUND_SINK];
    s.insert(s.end(), p, p + len);
    return true;
  }
};

class BLEClientService {
public:
  explicit BLEClientService(const BLEUuid&) {}
  void begin() {}
  bool discover(uint16_t) { return BleMock::discover_ok; }
};

class BLEClientCharacteristic {
public:
  explicit BLEClientCharacteristic(const BLEUuid&) {
    _idx = BleMock::client_chr_count;
    if (_idx < BleMock::MAX_SINKS) BleMock::client_chr[_idx] = this;
    BleMock::client_chr_count++;
  }
  void setNotifyCallback(void (*cb)(BLEClientCharacteristic*, uint8_t*, uint16_t)) {
    BleMock::notify_cb = cb;
  }
  void begin() {}
  bool discover() { return BleMock::discover_ok; }
  bool enableNotify() { return BleMock::discover_ok; }
  /* Reports what the stack ACCEPTED, which is what the real one does. */
  uint16_t write(const void* data, uint16_t len) {
    uint16_t n = BleMock::take(len);
    const uint8_t* p = (const uint8_t*)data;
    if (_idx < BleMock::MAX_SINKS) {
      std::vector<uint8_t>& s = BleMock::sink[_idx];
      s.insert(s.end(), p, p + n);
    }
    return n;
  }
  uint8_t _idx = 0;
};

class BluefruitCentral {
public:
  void setConnectCallback(void (*cb)(uint16_t)) { BleMock::connect_cb = cb; }
  void setDisconnectCallback(void (*cb)(uint16_t, uint8_t)) { BleMock::disconnect_cb = cb; }
  bool connect(const ble_gap_addr_t* addr) {
    BleMock::dial_addr = *addr;
    BleMock::dials++;
    return BleMock::connect_ok;
  }
};

/* The real BLEPeriph::connected(handle) is role AND liveness in one call. That
   is exactly what BleLink asks of it, so model both. */
class BluefruitPeriph {
public:
  bool connected(uint16_t h) {
    if (h >= BleMock::MAX_CONNS) return false;
    const BleMock::Conn& c = BleMock::conns[h];
    return c.used && c.connected && c.periph;
  }
  uint8_t connected() {
    uint8_t n = 0;
    for (uint16_t h = 0; h < BleMock::MAX_CONNS; h++) if (connected(h)) n++;
    return n;
  }
};

class BluefruitClass {
public:
  BluefruitCentral Central;
  BluefruitPeriph Periph;
  BLEConnection* Connection(uint16_t h) {
    if (h >= BleMock::MAX_CONNS || !BleMock::conns[h].used) return nullptr;
    BleMock::wrapper[h].handle = h;
    return &BleMock::wrapper[h];
  }
};

inline BluefruitClass Bluefruit;

namespace BleMock {

inline void reset() {
  now_ms = 1000;                       // not 0: BleLink reads 0 as "never"
  credits = -1;
  last_notify_conn = BLE_CONN_HANDLE_INVALID;
  discover_ok = true;
  connect_ok = true;
  dials = 0;
  memset(&dial_addr, 0, sizeof(dial_addr));
  for (uint8_t i = 0; i < MAX_CONNS; i++) conns[i] = Conn();
  for (uint8_t i = 0; i < MAX_SINKS; i++) sink[i].clear();
}

inline void advance(unsigned long ms) { now_ms += ms; }

inline void clearSinks() {
  for (uint8_t i = 0; i < MAX_SINKS; i++) sink[i].clear();
}

/** Open a connection to a peer and report its handle. A peer that dialled US
 *  puts us in the peripheral role, which is the default here. */
inline uint16_t openConn(const ble_gap_addr_t& peer, uint16_t mtu = 247,
                         bool periph = true) {
  for (uint16_t h = 0; h < MAX_CONNS; h++) {
    if (conns[h].used) continue;
    conns[h].used = true;
    conns[h].connected = true;
    conns[h].periph = periph;
    conns[h].peer = peer;
    conns[h].mtu = mtu;
    return h;
  }
  return BLE_CONN_HANDLE_INVALID;
}

/** Answer the dial that BleLink::loop() just made. We are the central here. */
inline uint16_t completeDial(uint16_t mtu = 247) {
  uint16_t h = openConn(dial_addr, mtu, false);
  if (connect_cb) connect_cb(h);
  return h;
}

/** A client that paired, which is what the CLI and DFU both need. */
inline void pair(uint16_t h) { conns[h].secured = true; conns[h].bonded = true; }

/** The stack has not torn the connection down yet, which is what an
 *  asynchronous disconnect looks like from the main loop. */
inline void disconnectInFlight(uint16_t h) { conns[h].connected = true; }

/** The peer disconnected, and the stack reported it. */
inline void peerDisconnect(uint16_t h) {
  conns[h].connected = false;
  if (disconnect_cb) disconnect_cb(h, 0x13);
}

/** The peer went away with no report, which is what a lost radio looks like. */
inline void vanish(uint16_t h) { conns[h].connected = false; }

/** Bytes arrive on an outward link, as a GATT notification. */
inline void notifyFrom(uint8_t link_idx, const uint8_t* data, uint16_t len) {
  if (notify_cb) notify_cb(client_chr[link_idx], (uint8_t*)data, len);
}

/** Bytes arrive from a peer that dialled US, as a GATT write. */
inline void writeFrom(uint16_t conn, const uint8_t* data, uint16_t len) {
  if (written_cb) written_cb(conn, nullptr, (uint8_t*)data, len);
}

}  // namespace BleMock
