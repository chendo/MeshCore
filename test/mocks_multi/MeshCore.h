#pragma once
#include <cstdint>
#define PUB_KEY_SIZE     32
#define PRV_KEY_SIZE     64
#define MAX_PATH_SIZE    64
#define MAX_TRANS_UNIT  255
// packet geometry, matching src/MeshCore.h — the trace recomputes MeshCore's
// own packet hash, so these have to agree with the firmware's
#define MAX_HASH_SIZE       8
#define CIPHER_BLOCK_SIZE  16
#define MAX_PACKET_PAYLOAD 184

// MESH_DEBUG is a firmware serial-logging flag; the real macros live in
// src/MeshCore.h, which this header shadows.
#define MESH_DEBUG_PRINT(...) {}
#define MESH_DEBUG_PRINTLN(...) {}

namespace mesh {
// The observer differences peers' advert timestamps against our own clock.
class RTCClock {
public:
  virtual ~RTCClock() {}
  virtual uint32_t getCurrentTime() { return 0; }
  virtual void setCurrentTime(uint32_t time) {}
};
}
