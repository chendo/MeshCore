#pragma once

// SimpleMeshTables fixes its dedup table at 160 hashes (1,280 B) via a #define,
// which is one table size for the whole firmware. A repeater needs that — it
// sees and forwards every flood on the band — but a chat identity only has to
// recognise repeats of the handful of packets addressed to it, so the same
// table there is ~1 KB of RAM per slot bought for nothing. Same logic, size as
// a template parameter.

#include <Mesh.h>
#include <string.h>

template <int N_HASHES>
class SlotMeshTables : public mesh::MeshTables {
  uint8_t _hashes[N_HASHES * MAX_HASH_SIZE];
  int _next_idx;
  uint32_t _direct_dups, _flood_dups;

public:
  SlotMeshTables() : _next_idx(0), _direct_dups(0), _flood_dups(0) {
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

  uint32_t getNumDirectDups() const { return _direct_dups; }
  uint32_t getNumFloodDups() const { return _flood_dups; }
};
