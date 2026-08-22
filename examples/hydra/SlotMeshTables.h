#pragma once

// SimpleMeshTables sets its dedup table to 160 hashes (1,280 B) with a #define.
// That gives one table size for the whole firmware. A repeater needs that size,
// because it sees and forwards every flood on the band. But a chat identity
// only has to recognise repeats of the few packets that are addressed to it.
// The same table there costs about 1 KB of RAM per slot for no gain. This class
// has the same logic, but the size is a template parameter.

#include <Mesh.h>
#include <string.h>

// The dedup counters sit in a base class that is NOT a template. A caller that
// only holds a mesh::MeshTables* can then read them without knowing the size of
// the table. The room server status reply needs exactly that.
class DupCountingTables : public mesh::MeshTables {
protected:
  uint32_t _direct_dups, _flood_dups;
public:
  DupCountingTables() : _direct_dups(0), _flood_dups(0) {}
  uint32_t getNumDirectDups() const { return _direct_dups; }
  uint32_t getNumFloodDups() const { return _flood_dups; }
};

template <int N_HASHES>
class SlotMeshTables : public DupCountingTables {
  uint8_t _hashes[N_HASHES * MAX_HASH_SIZE];
  int _next_idx;

public:
  SlotMeshTables() : _next_idx(0) {
    memset(_hashes, 0, sizeof(_hashes));
  }

  bool wasSeen(const mesh::Packet* packet) override {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    const uint8_t* sp = _hashes;
    for (int i = 0; i < N_HASHES; i++, sp += MAX_HASH_SIZE) {
      if (memcmp(hash, sp, MAX_HASH_SIZE) == 0) {
        if (packet->isRouteDirect()) _direct_dups++; else _flood_dups++;
        return true;
      }
    }
    return false;
  }

  void markSeen(const mesh::Packet* packet) override {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    memcpy(&_hashes[_next_idx * MAX_HASH_SIZE], hash, MAX_HASH_SIZE);
    _next_idx = (_next_idx + 1) % N_HASHES;
  }

  void clear(const mesh::Packet* packet) override {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    uint8_t* sp = _hashes;
    for (int i = 0; i < N_HASHES; i++, sp += MAX_HASH_SIZE) {
      if (memcmp(hash, sp, MAX_HASH_SIZE) == 0) { memset(sp, 0, MAX_HASH_SIZE); break; }
    }
  }
};
