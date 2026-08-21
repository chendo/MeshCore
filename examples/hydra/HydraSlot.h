#pragma once

// A hydra node runs several mesh identities over one LoRa radio. Each identity
// lives in a SLOT. A slot has its own keypair, its own name, its own mesh::Mesh,
// its own packet pool and its own dedup table. It also has its own RadioPort
// from the shared radio arbiter. A room slot has its own ACL as well.
//
// Slot 0 is the repeater. You cannot disable it. It is the reason that the node
// is at its location. Slots 1..N are optional and can be off.

#include "SlotPolicy.h"
#include <Mesh.h>
#include <new>
#include <helpers/IdentityStore.h>
#include <helpers/SharedRadio.h>
#include <helpers/StaticPoolPacketManager.h>

class HydraSlot {
public:
  virtual ~HydraSlot() {}

  // The node registers the port of every slot with the arbiter at boot. It does
  // this for an enabled slot and for a disabled slot. Thus port index == slot
  // index for the whole boot. The packet trace and the TX-owner records refer
  // to a port by its index.
  virtual RadioPort& port() = 0;

  // `id_name` is the STORAGE key. The code makes it from the slot index only.
  // It never makes it from the type or from the display name (decision 7).
  // `display_name` goes in the advert. It must not be empty at this point.
  virtual bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name,
                     const char* display_name, SlotType type) = 0;
  virtual void loop() = 0;

  virtual SlotType type() const = 0;
  virtual const mesh::LocalIdentity& identity() const = 0;
  virtual const char* name() const = 0;
  virtual void setName(const char* n) {}
  virtual bool hasPendingWork() const = 0;   // this controls the powersave sleep
  virtual void flushPendingWrites() {}       // the delayed ACL writes, before a reboot

  // `slot N <cmd>`. A sender_timestamp of 0 means the serial console. This is
  // the same convention as upstream for "the caller is physically present".
  virtual void handleCommand(uint32_t sender_timestamp, char* command,
                             char* reply, size_t reply_sz) = 0;
};

// The constructor allocates the packet pool on the heap. StaticPoolPacketManager
// news one mesh::Packet for each entry. But every slot object must exist at
// boot, so that the node can register its port. This class delays the
// allocation until begin(). A disabled slot then costs zero heap. And a slot
// that you enable after months of uptime asks the heap for one continuous
// block. It does not lose the memory to fragmentation piece by piece.
class DeferredPacketManager : public mesh::PacketManager {
  StaticPoolPacketManager* _p;
  int _pool_size;
public:
  explicit DeferredPacketManager(int pool_size) : _p(nullptr), _pool_size(pool_size) {}
  // False means that the heap refused. This is the node RAM reserve floor at
  // work. The caller turns this into a refusal to enable the slot.
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
