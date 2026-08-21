#pragma once
#include <cstdint>
#define PUB_KEY_SIZE     32
#define PRV_KEY_SIZE     64
#define MAX_PATH_SIZE    64
#define MAX_TRANS_UNIT  255
// The packet sizes agree with src/MeshCore.h. The trace calculates the
// MeshCore packet hash again. Thus these values must agree with the firmware.
#define MAX_HASH_SIZE       8
#define CIPHER_BLOCK_SIZE  16
#define MAX_PACKET_PAYLOAD 184

// MESH_DEBUG is a flag in the firmware for serial log output. The real macros
// are in src/MeshCore.h. This header takes the place of that file.
#define MESH_DEBUG_PRINT(...) {}
#define MESH_DEBUG_PRINTLN(...) {}

namespace mesh {
// The observer compares the advert timestamps of the peers with our own clock.
class RTCClock {
public:
  virtual ~RTCClock() {}
  virtual uint32_t getCurrentTime() { return 0; }
  virtual void setCurrentTime(uint32_t time) {}
};
}
