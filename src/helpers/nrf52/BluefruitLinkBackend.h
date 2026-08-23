#pragma once

#include <stdint.h>
#include <bluefruit.h>

#include "helpers/bridges/BleLinkTypes.h"
#include "BleStack.h"

/**
 * @brief  The Bluefruit half of the bridge peer link.
 *
 * BleLink holds the framing, the queues and the policy, and none of it names a
 * BLE stack. Everything that does name one is here. NimBleLinkBackend answers
 * the same calls on ESP32, and the build picks between them with
 * BLE_LINK_BACKEND_CLASS.
 *
 * Every method is static. The stack objects below are registered with the
 * SoftDevice once for the life of the process, so there is nothing per-instance
 * to hold, and a static call costs the link nothing.
 *
 * The bodies stay in this header on purpose: the compiler then sees the same
 * calls at the same places that it saw when they were written out in
 * BleLink.cpp, so the seam costs no flash.
 */
class BluefruitLinkBackend {
public:
  /** Delivered on an outward link, which the backend reports by slot index. */
  typedef void (*notify_cb_t)(uint8_t link_idx, const uint8_t* data, uint16_t len);
  /** Delivered by a peer that dialled IN, which has no slot. */
  typedef void (*written_cb_t)(uint16_t conn, const uint8_t* data, uint16_t len);
  typedef void (*conn_cb_t)(uint16_t conn);
  typedef void (*disconn_cb_t)(uint16_t conn, uint8_t reason);

  /** No connection. The SoftDevice value, so a handle needs no translation. */
  static const uint16_t NO_CONN = BLE_CONN_HANDLE_INVALID;
  /** Peripheral connections that sweepInbound() must walk. The SoftDevice
   *  numbers its handles from zero, so a handle IS the slot here. */
  static const uint8_t SWEEP_SLOTS = BLE_MAX_CONNECTION;

  /**
   * @brief  Register the GATT service and the callbacks.
   *
   * @param chunk  the largest single write, which sizes the characteristic.
   */
  static void begin(uint16_t chunk, conn_cb_t on_conn, disconn_cb_t on_disconn,
                    notify_cb_t on_notify, written_cb_t on_written);

  /** The ATT MTU that the stack negotiated. BleLink turns it into a write
   *  size; the ladder that produced it is in BleStack. */
  static uint16_t stackMtu() { return BleStack::mtu(); }

  /* ---- Connections ------------------------------------------------------ */

  /** True while the stack reports this handle and reports it as live. */
  static bool connected(uint16_t conn) {
    BLEConnection* c = Bluefruit.Connection(conn);
    return c != nullptr && c->connected();
  }
  /** True while the stack holds this handle at all, live or not. */
  static bool exists(uint16_t conn) { return Bluefruit.Connection(conn) != nullptr; }
  /** A no-op on a handle that the stack does not hold. */
  static void disconnect(uint16_t conn) {
    BLEConnection* c = Bluefruit.Connection(conn);
    if (c != nullptr) c->disconnect();
  }
  /** @returns false when the stack does not report the connection. */
  static bool peerAddr(uint16_t conn, BleAddr& out) {
    BLEConnection* c = Bluefruit.Connection(conn);
    if (c == nullptr) return false;
    out = c->getPeerAddr();
    return true;
  }
  /** The ATT MTU of one connection, or 0 when the stack reports none. */
  static uint16_t connMtu(uint16_t conn) {
    BLEConnection* c = Bluefruit.Connection(conn);
    return c == nullptr ? 0 : c->getMtu();
  }
  /** Secured or bonded, which means the CLI or DFU and never a bridge peer. */
  static bool paired(uint16_t conn) {
    BLEConnection* c = Bluefruit.Connection(conn);
    return c != nullptr && (c->secured() || c->bonded());
  }
  /**
   * @brief  The nth live connection on which we hold the PERIPHERAL role.
   *
   * Role and liveness together, so an outward link of our own never matches.
   * @returns NO_CONN when that slot holds nothing.
   */
  static uint16_t peripheralConnAt(uint8_t slot) {
    return Bluefruit.Periph.connected(slot) ? (uint16_t)slot : NO_CONN;
  }

  /** Nothing to deliver: every Bluefruit callback reaches BleLink directly,
   *  on a task that BleLink is already written for. */
  static void poll() {}

  /* ---- The central side ------------------------------------------------- */

  static bool dial(const BleAddr& addr) { return Bluefruit.Central.connect(&addr); }

  /** Abandon a connect that never completed.
   *
   *  Nothing to do here. The SoftDevice reports every connect attempt through
   *  the central connect callback, success or failure, so a dial on this
   *  backend always ends and BleLink's timeout never has to reach for this.
   *  It exists because the ESP32 backend does need it: NimBLE can leave a
   *  connect outstanding for ever, and an outstanding connect blocks the
   *  scanner. Bluefruit offers no stopConnecting(), so say so honestly rather
   *  than pretend the attempt was cancelled. */
  static bool cancelDial() { return false; }

  /** Find our service on an outward link and subscribe to it. */
  static bool discoverLink(uint8_t idx, uint16_t conn);

  /* ---- Data ------------------------------------------------------------- */

  /**
   * @brief  Write to one outward link.
   * @returns the bytes that the stack ACCEPTED, which can be fewer than asked.
   */
  static uint16_t writeLink(uint8_t idx, const uint8_t* data, uint16_t len);

  /**
   * @brief  Notify the peer that dialled in, on which the roles are reversed.
   *
   * Named by connection, and not "every subscriber": there is one inbound peer,
   * and a stranger that connects and subscribes must not be handed our frames.
   *
   * @returns false when the stack took nothing. It reports only a bool, which
   *          is why BleLink keeps every notify to a single packet.
   */
  static bool notifyInbound(uint16_t conn, const uint8_t* data, uint16_t len);
};
