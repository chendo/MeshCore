#pragma once

// A hydra node runs several mesh identities over one LoRa radio. Each identity
// lives in a SLOT: its own keypair, its own name, its own mesh::Mesh, its own
// packet pool and dedup table, and its own RadioPort out of the shared radio
// arbiter. A room slot also has its own ACL.
//
// Slot 0 is the repeater and is not disableable — it is the reason the node is
// sited where it is. Slots 1..N are optional and may be off.

#include "SlotPolicy.h"
#include <Mesh.h>
#include <new>
#include <helpers/IdentityStore.h>
#include <helpers/SharedRadio.h>
#include <helpers/StaticPoolPacketManager.h>

class HydraSlot {
public:
  virtual ~HydraSlot() {}

  // Every slot's port is registered with the arbiter at boot, enabled or not,
  // so port index == slot index for the life of the boot: the packet trace and
  // the TX-owner bookkeeping reference ports by index.
  virtual RadioPort& port() = 0;

  // `id_name` is the STORAGE key and is derived from the slot index alone —
  // never from the type or the display name (decision 7). `display_name` is
  // what goes in the advert and must be non-empty by the time we get here.
  virtual bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name,
                     const char* display_name, SlotType type) = 0;
  virtual void loop() = 0;

  virtual SlotType type() const = 0;
  virtual const mesh::LocalIdentity& identity() const = 0;
  virtual const char* name() const = 0;
  virtual void setName(const char* n) {}
  virtual bool hasPendingWork() const = 0;   // gates the node's powersave sleep
  virtual void flushPendingWrites() {}       // lazy ACL writes, before a reboot

  // `slot N <cmd>`. sender_timestamp 0 means the serial console, matching
  // upstream's convention for "this caller is physically present".
  virtual void handleCommand(uint32_t sender_timestamp, char* command,
                             char* reply, size_t reply_sz) = 0;
};

// The packet pool is heap-allocated at construction (StaticPoolPacketManager
// news one mesh::Packet per entry), but every slot object has to exist at boot
// so its port can be registered. Deferring the allocation to begin() keeps a
// disabled slot's heap cost at zero, and means a slot enabled after months of
// uptime asks the heap for one contiguous block rather than losing to
// fragmentation piecemeal.
class DeferredPacketManager : public mesh::PacketManager {
  StaticPoolPacketManager* _p;
  int _pool_size;
public:
  explicit DeferredPacketManager(int pool_size) : _p(nullptr), _pool_size(pool_size) {}
  // False means the heap refused, which is the node RAM reserve floor doing
  // its job — the caller turns that into a refusal to enable the slot.
  bool allocatePool() {
    if (_p == nullptr) _p = new (std::nothrow) StaticPoolPacketManager(_pool_size);
    return _p != nullptr;
  }

  mesh::Packet* allocNew() override { return _p ? _p->allocNew() : NULL; }
  void free(mesh::Packet* packet) override { if (_p) _p->free(packet); }
  void queueOutbound(mesh::Packet* p, uint8_t pri, uint32_t at) override { if (_p) _p->queueOutbound(p, pri, at); }
  mesh::Packet* getNextOutbound(uint32_t now) override { return _p ? _p->getNextOutbound(now) : NULL; }
  int getOutboundCount(uint32_t now) const override { return _p ? _p->getOutboundCount(now) : 0; }
  int getOutboundTotal() const override { return _p ? _p->getOutboundTotal() : 0; }
  int getFreeCount() const override { return _p ? _p->getFreeCount() : 0; }
  mesh::Packet* getOutboundByIdx(int i) override { return _p ? _p->getOutboundByIdx(i) : NULL; }
  mesh::Packet* removeOutboundByIdx(int i) override { return _p ? _p->removeOutboundByIdx(i) : NULL; }
  void queueInbound(mesh::Packet* p, uint32_t at) override { if (_p) _p->queueInbound(p, at); }
  mesh::Packet* getNextInbound(uint32_t now) override { return _p ? _p->getNextInbound(now) : NULL; }
};
