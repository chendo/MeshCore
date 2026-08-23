#pragma once

/* A module that an entrypoint drives, instead of an entrypoint that names every
 * module it drives.
 *
 * Today every example's main.cpp hardcodes its own list -- `the_mesh.loop();
 * sensors.loop(); ui_task.loop();` -- so adding a feature to a node means
 * editing the entrypoint, and a feature that lives in a fork means editing an
 * upstream file to reach it. A module registers itself instead, and main.cpp
 * gains three calls that never change again.
 *
 * SCOPE IS PART OF THE INTERFACE, not something to add later.
 *
 *   NODE  owns the board: watchdogs, an LED, sensors, a console, a link. One of
 *         each, whatever else the node runs.
 *   MESH  belongs to one identity: it sees that identity's packets, and it is
 *         told WHICH identity, because a node can run several at once.
 *
 * Retrofitting that distinction would mean touching every module, so the packet
 * callbacks carry a mesh index from the start even where only one mesh exists.
 * On a single-identity node it is always 0 and costs nothing.
 *
 * No heap and no ordering surprises: the registry is a fixed array, and modules
 * run in registration order. Registration is expected at static-init or in
 * setup(), before begin(); adding one later is allowed but a module that
 * registers after setupAll() will not get its onSetup().
 */

#include <Arduino.h>

/* Forward declaration, deliberately. A module that never looks at a packet
   should not have to compile the mesh to say so, and the interface only ever
   passes the pointer through. */
namespace mesh { class Packet; }

#ifndef APP_MODULE_MAX
  #define APP_MODULE_MAX 8
#endif

enum class AppScope : uint8_t { NODE, MESH };

class AppModule {
public:
  virtual ~AppModule() {}

  /** NODE or MESH. Fixed for the life of the module. */
  virtual AppScope scope() const = 0;

  /** Called once, in registration order, from AppModules::setupAll(). */
  virtual void onSetup() {}

  /** Called on every pass of the main loop. Must not block: the node's LoRa
   *  timing and its watchdogs share this thread. */
  virtual void onLoop() {}

  /** True while this module still has work that a sleep would lose. The node
   *  will not enter powersave while any module says true. */
  virtual bool hasPendingWork() const { return false; }

  /** MESH scope only. `mesh_idx` names the identity the packet belongs to.
   *  Driven from the existing logTx/logRx seam, so a module sees exactly what
   *  the bridge and the observer see, at the same point. */
  virtual void onPacketSent(uint8_t mesh_idx, mesh::Packet* pkt, int len) {
    (void)mesh_idx; (void)pkt; (void)len;
  }
  virtual void onPacketRecv(uint8_t mesh_idx, mesh::Packet* pkt, int len, float score) {
    (void)mesh_idx; (void)pkt; (void)len; (void)score;
  }
};

class AppModules {
  static AppModule* _mods[APP_MODULE_MAX];
  static uint8_t _count;
  static bool _setup_done;

public:
  /** @returns false when the registry is full, so a build that adds one module
   *  too many fails visibly rather than silently dropping it. */
  static bool add(AppModule* m) {
    if (m == nullptr || _count >= APP_MODULE_MAX) return false;
    _mods[_count++] = m;
    return true;
  }

  static uint8_t count() { return _count; }

  /** Empty the registry. A node never unregisters -- it registers once at boot
   *  and runs until it reboots -- so this exists for host tests, which share
   *  one process across cases, and for a re-init that starts from nothing. */
  static void reset() { _count = 0; _setup_done = false; }
  static AppModule* at(uint8_t i) { return i < _count ? _mods[i] : nullptr; }

  static void setupAll() {
    for (uint8_t i = 0; i < _count; i++) _mods[i]->onSetup();
    _setup_done = true;
  }

  static void loopAll() {
    for (uint8_t i = 0; i < _count; i++) _mods[i]->onLoop();
  }

  static bool anyPendingWork() {
    for (uint8_t i = 0; i < _count; i++) {
      if (_mods[i]->hasPendingWork()) return true;
    }
    return false;
  }

  /* Packet events reach MESH-scoped modules only. A node module that wanted
     them would be asking the wrong question: it would have to pick an identity,
     and the point of node scope is that it has none. */
  static void packetSent(uint8_t mesh_idx, mesh::Packet* pkt, int len) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_mods[i]->scope() == AppScope::MESH) _mods[i]->onPacketSent(mesh_idx, pkt, len);
    }
  }
  static void packetRecv(uint8_t mesh_idx, mesh::Packet* pkt, int len, float score) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_mods[i]->scope() == AppScope::MESH) _mods[i]->onPacketRecv(mesh_idx, pkt, len, score);
    }
  }
};
