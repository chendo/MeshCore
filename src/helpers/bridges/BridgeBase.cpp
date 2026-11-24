#include "BridgeBase.h"

#ifdef CLOCK_CONVERGE_MS
  #include "helpers/MeshObserver.h"
  MeshObserver& rfObserver();     // helpers/ObservingWrapper.cpp
#endif

#include <Arduino.h>

bool BridgeBase::isRunning() const {
  return _initialized;
}

const char *BridgeBase::getLogDateTime() {
  static char tmp[32];
  uint32_t now = _rtc->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

uint16_t BridgeBase::fletcher16(const uint8_t *data, size_t len) {
  uint8_t sum1 = 0, sum2 = 0;

  for (size_t i = 0; i < len; i++) {
    sum1 = (sum1 + data[i]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
}

bool BridgeBase::validateChecksum(const uint8_t *data, size_t len, uint16_t received_checksum) {
  uint16_t calculated_checksum = fletcher16(data, len);
  return received_checksum == calculated_checksum;
}

void BridgeBase::handleReceivedPacket(mesh::Packet *packet) {
  // Guard against uninitialized state
  if (_initialized == false) {
    BRIDGE_DEBUG_PRINTLN("RX packet received before initialization\n");
    _mgr->free(packet);
    return;
  }

  if (!_seen_packets.wasSeen(packet)) {
    _seen_packets.markSeen(packet);
#ifdef CLOCK_CONVERGE_MS
    /* Clock samples only -- observeBridgedAdvert() deliberately touches no peer
       table, hop histogram or SNR statistic, because our radio never heard this
       and a neighbour list must not fill with nodes on the other band.
       This is the whole reason a bridge node can hold a clock at all: a node
       alone on its band overhears no adverts, so the ones its bridge peer
       relays are its ONLY evidence of the time. See MeshObserver.h. */
    {
      uint8_t frame[MAX_TRANS_UNIT];
      int n = packet->writeTo(frame);
      if (n > 0) rfObserver().observeBridgedAdvert(frame, n);
    }
#endif
    // bridge_delay provides a buffer to prevent immediate processing conflicts in the mesh network.
    _mgr->queueInbound(packet, millis() + _prefs->bridge_delay);
  } else {
    _mgr->free(packet);
  }
}
