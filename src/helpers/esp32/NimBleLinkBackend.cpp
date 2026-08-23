#include "NimBleLinkBackend.h"

#include "NimBleStack.h"

#include <NimBLEDevice.h>
#include <string.h>

static_assert(NimBleLinkBackend::NO_CONN == BLE_HS_CONN_HANDLE_NONE,
              "the link's no-connection value must be NimBLE's own");

/* The bridge service and its characteristic, as the nRF52 side declares them.
   The Bluefruit backend writes the same two UUIDs as little-endian byte arrays,
   which is the order that stack takes; these strings are the same 128 bits read
   the way a person writes a UUID. They must not drift apart: they are the wire
   format, and a mismatch makes a ThinkNode M1 and a ThinkNode M5 connect and
   then find nothing to talk to. */
static const char* BRIDGE_SVC_UUID = "6e407a01-b5a3-f393-e0a9-e50e24dcca9e";
static const char* BRIDGE_CHR_UUID = "6e407a02-b5a3-f393-e0a9-e50e24dcca9e";

static NimBleLinkBackend::conn_cb_t    s_on_conn = nullptr;
static NimBleLinkBackend::disconn_cb_t s_on_disconn = nullptr;
static NimBleLinkBackend::notify_cb_t  s_on_notify = nullptr;
static NimBleLinkBackend::written_cb_t s_on_written = nullptr;

/* Peripheral side: what a peer that dials US talks to. */
static NimBLEServer*         s_server = nullptr;
static NimBLECharacteristic* s_chr = nullptr;

/* Central side. One client for each outward link, made only when a link first
   dials, because a client that nothing uses still costs RAM.

   Both arrays are written from the MAIN LOOP only -- by discoverLink() and by
   dial() -- and read from the host task by the notify trampoline. That is
   deliberate. A remote characteristic belongs to its client and is freed by
   connect(), so an entry cleared from the host task on disconnect would race a
   write on the main loop, and an entry left in place would let a later
   allocation land on the same address and send one link's frames to another's
   slot. dial() clears the entry just before connect() frees the object, which
   closes both. */
static NimBLEClient*               s_link_client[BLE_LINK_MAX_LINKS] = { nullptr };
static NimBLERemoteCharacteristic* s_link_chr[BLE_LINK_MAX_LINKS] = { nullptr };

/* The peripheral connections, which sweepInbound() walks. NimBLE gives out
   connection handles that a caller cannot enumerate, and NimBLEServer's own
   index accessor mis-indexes a list with a hole in it, so keep the list here.
   Written from the host task, read from the main loop. */
static volatile uint16_t s_periph[NimBleLinkBackend::SWEEP_SLOTS];

static bool s_gatt_ready = false;

/* ---- Connect and disconnect, held for the main loop ---------------------- */

/* A NimBLE callback runs on the host task, and a blocking host call from there
   waits for a reply that the same task must deliver. BleLink runs its GATT
   discovery inside its connect handler, which is right on nRF52 and fatal here,
   so the event waits in this ring until poll() releases it on the main loop.

   One producer, one consumer, so no lock is needed: the writer fills a slot and
   then publishes it by advancing the head, and the fence keeps the compiler and
   the core from reordering those two. */
struct LinkEvent {
  uint16_t conn;
  uint8_t reason;
  bool is_disconnect;
};
static const uint8_t EVENTQ_DEPTH = 8;
static LinkEvent s_evq[EVENTQ_DEPTH];
static volatile uint8_t s_evq_head = 0;
static volatile uint8_t s_evq_tail = 0;

static void pushEvent(uint16_t conn, bool is_disconnect, uint8_t reason) {
  uint8_t head = s_evq_head;
  uint8_t next = (uint8_t)((head + 1) % EVENTQ_DEPTH);
  if (next == s_evq_tail) return;               // full: the loop has stalled
  s_evq[head].conn = conn;
  s_evq[head].is_disconnect = is_disconnect;
  s_evq[head].reason = reason;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  s_evq_head = next;
}

void NimBleLinkBackend::poll() {
  while (s_evq_tail != s_evq_head) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    LinkEvent e = s_evq[s_evq_tail];
    s_evq_tail = (uint8_t)((s_evq_tail + 1) % EVENTQ_DEPTH);
    if (e.is_disconnect) {
      if (s_on_disconn) s_on_disconn(e.conn, e.reason);
    } else {
      if (s_on_conn) s_on_conn(e.conn);
    }
  }
}

/* ---- Callbacks ---------------------------------------------------------- */

class LinkClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override { pushEvent(c->getConnHandle(), false, 0); }
  void onDisconnect(NimBLEClient* c, int reason) override {
    pushEvent(c->getConnHandle(), true, (uint8_t)reason);
  }
  /* A dial that never completed. BleLink holds the slot in CONNECTING until
     something reports a handle, so say nothing here: the handle we would
     report is NO_CONN, which matches no link and could match the wrong one. */
  void onConnectFail(NimBLEClient* c, int reason) override { (void)c; (void)reason; }
};
static LinkClientCallbacks s_client_cbs;

class LinkServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* srv, NimBLEConnInfo& info) override {
    (void)srv;
    for (uint8_t i = 0; i < NimBleLinkBackend::SWEEP_SLOTS; i++) {
      if (s_periph[i] == NimBleLinkBackend::NO_CONN) {
        s_periph[i] = info.getConnHandle();
        return;
      }
    }
  }
  void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& info, int reason) override {
    (void)srv; (void)reason;
    for (uint8_t i = 0; i < NimBleLinkBackend::SWEEP_SLOTS; i++) {
      if (s_periph[i] == info.getConnHandle()) s_periph[i] = NimBleLinkBackend::NO_CONN;
    }
  }
};
static LinkServerCallbacks s_server_cbs;

class LinkCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    if (s_on_written == nullptr) return;
    /* One copy of the chunk. NimBLE keeps the written value inside the
       characteristic and exposes it only BY VALUE, so there is no way to hand
       BleLink a pointer into it. A chunk is at most 244 bytes and a bridge link
       carries a few frames a second, so the copy is cheap. It is called out
       because it is a heap allocation on the host task: if one ever fails, the
       cost is one lost chunk and the SYNC hunt recovers on the next frame. */
    NimBLEAttValue v = chr->getValue();
    s_on_written(info.getConnHandle(), v.data(), v.length());
  }
};
static LinkCharCallbacks s_chr_cbs;

static void notify_trampoline(NimBLERemoteCharacteristic* chr, uint8_t* data,
                              size_t len, bool is_notify) {
  (void)is_notify;
  if (s_on_notify == nullptr || chr == nullptr) return;
  for (uint8_t i = 0; i < BLE_LINK_MAX_LINKS; i++) {
    if (chr == s_link_chr[i]) { s_on_notify(i, data, (uint16_t)len); return; }
  }
}

/* ---- Lifecycle ---------------------------------------------------------- */

void NimBleLinkBackend::begin(uint16_t chunk, conn_cb_t on_conn, disconn_cb_t on_disconn,
                              notify_cb_t on_notify, written_cb_t on_written) {
  s_on_conn = on_conn;
  s_on_disconn = on_disconn;
  s_on_notify = on_notify;
  s_on_written = on_written;

  if (s_gatt_ready) return;
  s_gatt_ready = true;

  for (uint8_t i = 0; i < SWEEP_SLOTS; i++) s_periph[i] = NO_CONN;

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(&s_server_cbs, false);
  /* The discovery class is the ONE owner of the advertiser, exactly as on
     nRF52. A second owner that restarts advertising on every disconnect makes
     the node stop to advertise while it bridges perfectly well, which leaves a
     repeater with no cable unreachable. */
  s_server->advertiseOnDisconnect(false);

  NimBLEService* svc = s_server->createService(BRIDGE_SVC_UUID);
  s_chr = svc->createCharacteristic(BRIDGE_CHR_UUID,
                                    NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,
                                    chunk);
  s_chr->setCallbacks(&s_chr_cbs);
  svc->start();
  s_server->start();
}

uint16_t NimBleLinkBackend::stackMtu() { return NimBleStack::mtu(); }

/* ---- Connections -------------------------------------------------------- */

static bool connDesc(uint16_t conn, ble_gap_conn_desc& d) {
  if (conn == NimBleLinkBackend::NO_CONN) return false;
  return ble_gap_conn_find(conn, &d) == 0;
}

bool NimBleLinkBackend::connected(uint16_t conn) {
  ble_gap_conn_desc d;
  return connDesc(conn, d);
}

void NimBleLinkBackend::disconnect(uint16_t conn) {
  if (conn == NO_CONN) return;
  /* An outward link belongs to a client and the inbound peer to the server.
     Ask the client list first: it is short, and a handle can be in only one. */
  NimBLEClient* c = NimBLEDevice::getClientByHandle(conn);
  if (c != nullptr) { c->disconnect(); return; }
  if (s_server != nullptr) s_server->disconnect(conn);
}

bool NimBleLinkBackend::peerAddr(uint16_t conn, BleAddr& out) {
  ble_gap_conn_desc d;
  if (!connDesc(conn, d)) return false;
  memcpy(out.addr, d.peer_ota_addr.val, 6);
  out.addr_type = d.peer_ota_addr.type;
  out.addr_id_peer = 0;
  return true;
}

uint16_t NimBleLinkBackend::connMtu(uint16_t conn) {
  if (conn == NO_CONN) return 0;
  return ble_att_mtu(conn);                     // 0 when there is no such connection
}

bool NimBleLinkBackend::paired(uint16_t conn) {
  ble_gap_conn_desc d;
  if (!connDesc(conn, d)) return false;
  return d.sec_state.encrypted || d.sec_state.bonded;
}

uint16_t NimBleLinkBackend::peripheralConnAt(uint8_t slot) {
  if (slot >= SWEEP_SLOTS) return NO_CONN;
  uint16_t h = s_periph[slot];
  if (h == NO_CONN) return NO_CONN;
  /* Liveness as well as role. The list is kept by the server callbacks, and a
     disconnect that has not been reported yet would otherwise look live. */
  ble_gap_conn_desc d;
  if (!connDesc(h, d)) return NO_CONN;
  return d.role == BLE_GAP_ROLE_SLAVE ? h : NO_CONN;
}

uint8_t NimBleLinkBackend::numPeripheralConns() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < SWEEP_SLOTS; i++) {
    if (peripheralConnAt(i) != NO_CONN) n++;
  }
  return n;
}

/* ---- The central side --------------------------------------------------- */

bool NimBleLinkBackend::dial(const BleAddr& addr) {
  NimBLEClient* c = NimBLEDevice::getDisconnectedClient();
  if (c == nullptr) {
    if (NimBLEDevice::getCreatedClientCount() >= BLE_LINK_MAX_LINKS) return false;
    c = NimBLEDevice::createClient();
    if (c == nullptr) return false;
    c->setClientCallbacks(&s_client_cbs, false);
    /* Give up on a peer that does not answer, instead of holding the one
       outstanding connect that NimBLE allows. BleLink's own backoff then dials
       again. Its state machine already tolerates an attempt that never
       completes, so this bounds how long that can last. */
    c->setConnectTimeout(10000);
  }

  /* Forget this client's attributes BEFORE connect() frees them. Done here, on
     the main loop, so nothing can be reading the pointer at the same moment. */
  for (uint8_t i = 0; i < BLE_LINK_MAX_LINKS; i++) {
    if (s_link_client[i] == c) s_link_chr[i] = nullptr;
  }

  NimBLEAddress peer(addr.addr, addr.addr_type);
  /* ASYNCHRONOUS. A blocking connect would stall the main loop for as long as
     the peer takes to answer, which stalls the mesh as well. The result arrives
     as a queued event and reaches BleLink from poll(). */
  return c->connect(peer, true, true, true);
}

bool NimBleLinkBackend::discoverLink(uint8_t idx, uint16_t conn) {
  if (idx >= BLE_LINK_MAX_LINKS) return false;
  NimBLEClient* c = NimBLEDevice::getClientByHandle(conn);
  if (c == nullptr) return false;
  s_link_client[idx] = c;
  s_link_chr[idx] = nullptr;

  NimBLERemoteService* svc = c->getService(BRIDGE_SVC_UUID);
  if (svc == nullptr) return false;
  NimBLERemoteCharacteristic* chr = svc->getCharacteristic(BRIDGE_CHR_UUID);
  if (chr == nullptr) return false;

  /* Bind the characteristic BEFORE we subscribe. A notification can arrive the
     instant the peer sees the CCCD write, and the trampoline finds the link by
     this pointer; unset, the first frame of the link would be dropped. */
  s_link_chr[idx] = chr;
  if (!chr->subscribe(true, notify_trampoline)) {
    s_link_chr[idx] = nullptr;
    return false;
  }
  return true;
}

/* ---- Data --------------------------------------------------------------- */

uint16_t NimBleLinkBackend::writeLink(uint8_t idx, const uint8_t* data, uint16_t len) {
  if (idx >= BLE_LINK_MAX_LINKS) return 0;
  NimBLERemoteCharacteristic* chr = s_link_chr[idx];
  if (chr == nullptr) return 0;
  /* Write Command, with no response. BleLink has already clamped len to this
     connection's ATT payload, so NimBLE takes the single-packet path and the
     result is all or nothing -- which is what BleLink's queue assumes, and what
     stops a part-sent frame going out twice.

     On a link that has just dropped this fails and returns 0, which BleLink
     reads as no credit. It resumes from the same offset, and the disconnect
     reaches it on the next poll(). */
  return chr->writeValue(data, len, false) ? len : 0;
}

bool NimBleLinkBackend::notifyInbound(uint16_t conn, const uint8_t* data, uint16_t len) {
  if (s_chr == nullptr || conn == NO_CONN) return false;
  /* To THIS peer, and not to every connected one. NimBLE's notify-to-all path
     sends to a peer whether or not it subscribed, so naming the connection is
     what keeps a stranger that connects and says nothing from being handed our
     frames. */
  return s_chr->notify(data, len, conn);
}
