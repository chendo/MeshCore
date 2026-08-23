#pragma once

#include <stdint.h>
/* BleLink measures every one of its timers with millis(). The backend header is
   where that comes from, because BleLink itself includes no platform header. */
#include <Arduino.h>

#include "helpers/bridges/BleLinkTypes.h"

/**
 * @brief  The NimBLE half of the bridge peer link, for ESP32.
 *
 * The counterpart to BluefruitLinkBackend. BleLink holds the framing, the
 * queues and the policy and is shared; this file is every call into NimBLE that
 * the link needs. The GATT service, the characteristic, the frame layout and
 * every timer match the nRF52 side exactly, so a ThinkNode M1 and a ThinkNode
 * M5 join one bridge group.
 *
 * BLE is the only radio both families carry. An nRF52 board has no WiFi and so
 * cannot reach the ESP-NOW bridge, which is why this backend exists.
 *
 * NimBLE, and not Bluedroid. Two reasons, in this order:
 *
 *  1. Bluedroid links about 94KB of unused ESP-BLE-MESH models through the
 *     static dispatch table in btc_task.c, on one call to createServer(). A
 *     bridge that costs a third of the free flash on an M5 is not shippable.
 *  2. The two host stacks cannot be present in one binary at all, so a build
 *     chooses. Confining NimBLE to the bridge env means no env that works
 *     today can regress.
 *
 * NOTHING in this file may block for long. See poll(): a NimBLE callback runs
 * on the host task, and a blocking host call from there waits for a reply that
 * the same task must deliver. That is why the connect and disconnect events are
 * queued and handed to BleLink from the main loop.
 */
class NimBleLinkBackend {
public:
  /** Delivered on an outward link, which the backend reports by slot index. */
  typedef void (*notify_cb_t)(uint8_t link_idx, const uint8_t* data, uint16_t len);
  /** Delivered by a peer that dialled IN, which has no slot. */
  typedef void (*written_cb_t)(uint16_t conn, const uint8_t* data, uint16_t len);
  typedef void (*conn_cb_t)(uint16_t conn);
  typedef void (*disconn_cb_t)(uint16_t conn, uint8_t reason);

  /** No connection. BLE_HS_CONN_HANDLE_NONE, which this header must not need
   *  NimBLE to state. Static-asserted against it in the .cpp. */
  static const uint16_t NO_CONN = 0xFFFF;

  /** Peripheral connections that sweepInbound() must walk. NimBLE hands out
   *  connection handles that a caller cannot enumerate, so the backend keeps
   *  the peripheral ones in a list of its own and this is its length. */
  static const uint8_t SWEEP_SLOTS = 4;

  /** Register the GATT service and the callbacks. @param chunk sizes the
   *  characteristic, which must hold one whole write. */
  static void begin(uint16_t chunk, conn_cb_t on_conn, disconn_cb_t on_disconn,
                    notify_cb_t on_notify, written_cb_t on_written);

  /**
   * @brief  Deliver connect and disconnect events on the caller's task.
   *
   * A NimBLE callback runs on the host task. GATT discovery is a blocking host
   * call, so to run it from a callback waits on a reply that the blocked task
   * itself has to deliver. BleLink does its discovery inside its connect
   * handler, exactly as it does on nRF52, so the event is queued here and
   * released from BleLink::loop() instead. The main loop may block; the host
   * task may not.
   */
  static void poll();

  /** The ATT MTU that this node asks for. */
  static uint16_t stackMtu();

  /* ---- Connections ------------------------------------------------------ */

  static bool connected(uint16_t conn);
  /** The same question on NimBLE: a handle the host does not hold is gone. */
  static bool exists(uint16_t conn) { return connected(conn); }
  static void disconnect(uint16_t conn);
  static bool peerAddr(uint16_t conn, BleAddr& out);
  /** The ATT MTU of one connection, or 0 when there is no such connection. */
  static uint16_t connMtu(uint16_t conn);
  /** Encrypted or bonded, which a bridge peer never is. */
  static bool paired(uint16_t conn);
  /** The nth live connection on which we hold the PERIPHERAL role, or NO_CONN.
   *  Role and liveness together, so an outward link never matches. */
  static uint16_t peripheralConnAt(uint8_t slot);

  /* ---- The central side ------------------------------------------------- */

  /** Start a connection. ASYNCHRONOUS: the result arrives through poll(), the
   *  same way the SoftDevice reports it through a callback. */
  static bool dial(const BleAddr& addr);

  /** Abandon a connect that never completed, and free the radio.
   *
   *  NimBLE can leave an async connect outstanding indefinitely, and while
   *  ble_gap_master.op is BLE_GAP_OP_M_CONN every ble_gap_disc() returns
   *  BLE_HS_EBUSY. One dial that never answers therefore makes the node
   *  permanently deaf: it cannot scan, so it never sees a beacon again. */
  static bool cancelDial();

  /** Find our service on an outward link and subscribe to it. Blocking, and
   *  safe, because BleLink reaches it from poll() and thus from the caller's
   *  own task. */
  static bool discoverLink(uint8_t idx, uint16_t conn);

  /* ---- Data ------------------------------------------------------------- */

  /**
   * @brief  Write to one outward link.
   *
   * @returns len when the write went out and 0 when it did not. NimBLE reports
   *          a bool, so there is no partial count to return -- and there is no
   *          partial write either, because BleLink clamps every write to the
   *          connection's ATT payload and NimBLE then sends it as one Write
   *          Command. All or nothing is what BleLink's queue assumes.
   */
  static uint16_t writeLink(uint8_t idx, const uint8_t* data, uint16_t len);

  /** Notify the peer that dialled in, on which the roles are reversed. */
  static bool notifyInbound(uint16_t conn, const uint8_t* data, uint16_t len);

  /* ---- For the discovery layer ------------------------------------------ */

  /** The peripheral connections in use, for the advert arbiter. */
  static uint8_t numPeripheralConns();
};
