#include "RS232Bridge.h"

#include <HardwareSerial.h>

#if !defined(RS232_BRIDGE_SERIAL) || !defined(RS232_BRIDGE_RX) || !defined(RS232_BRIDGE_TX)
#error "RS232_BRIDGE_SERIAL, RS232_BRIDGE_RX and RS232_BRIDGE_TX must be defined"
#endif

void RS232Bridge::begin() {
  BRIDGE_DEBUG_PRINTLN("Initializing at %d baud...\n", _prefs->bridge_baud);

#if defined(ESP32)
  RS232_BRIDGE_SERIAL.setPins(RS232_BRIDGE_RX, RS232_BRIDGE_TX);
#elif defined(NRF52_PLATFORM)
  // Tested with RAK_4631 and T114
  RS232_BRIDGE_SERIAL.setPins(RS232_BRIDGE_RX, RS232_BRIDGE_TX);
#elif defined(RP2040_PLATFORM)
  RS232_BRIDGE_SERIAL.setRX(RS232_BRIDGE_RX);
  RS232_BRIDGE_SERIAL.setTX(RS232_BRIDGE_TX);
#elif defined(STM32_PLATFORM)
  RS232_BRIDGE_SERIAL.setRx(RS232_BRIDGE_RX);
  RS232_BRIDGE_SERIAL.setTx(RS232_BRIDGE_TX);
#else
#error RS232Bridge was not tested on the current platform
#endif
  RS232_BRIDGE_SERIAL.begin(_prefs->bridge_baud);

  // Update bridge state
  _initialized = true;
}

void RS232Bridge::end() {
  BRIDGE_DEBUG_PRINTLN("Stopping...\n");
  RS232_BRIDGE_SERIAL.end();

  // Update bridge state
  _initialized = false;
}

void RS232Bridge::loop() {
  // Guard against uninitialized state
  if (_initialized == false) {
    return;
  }

  while (RS232_BRIDGE_SERIAL.available()) {
    uint8_t b = RS232_BRIDGE_SERIAL.read();

    if (_rx_buffer_pos < 2) {
      // Waiting for magic word
      if ((_rx_buffer_pos == 0 && b == ((BRIDGE_PACKET_MAGIC >> 8) & 0xFF)) ||
          (_rx_buffer_pos == 1 && b == (BRIDGE_PACKET_MAGIC & 0xFF))) {
        _rx_buffer[_rx_buffer_pos++] = b;
      } else {
        // Invalid magic byte, reset and start over
        _rx_buffer_pos = 0;
        // Check if this byte could be the start of a new magic word
        if (b == ((BRIDGE_PACKET_MAGIC >> 8) & 0xFF)) {
          _rx_buffer[_rx_buffer_pos++] = b;
        }
      }
    } else {
      // Reading length, payload, and checksum
      _rx_buffer[_rx_buffer_pos++] = b;

      if (_rx_buffer_pos >= 4) {
        uint16_t len = (_rx_buffer[2] << 8) | _rx_buffer[3];

        // Validate length field
        if (len > (MAX_TRANS_UNIT + 1)) {
          BRIDGE_DEBUG_PRINTLN("RX invalid length %d, resetting\n", len);
          _rx_buffer_pos = 0; // Invalid length, reset
          continue;
        }

        if (_rx_buffer_pos == len + SERIAL_OVERHEAD) { // Full packet received
          uint16_t received_checksum = (_rx_buffer[4 + len] << 8) | _rx_buffer[5 + len];

          if (validateChecksum(_rx_buffer + 4, len, received_checksum)) {
            BRIDGE_DEBUG_PRINTLN("RX, len=%d crc=0x%04x\n", len, received_checksum);
            mesh::Packet *pkt = _mgr->allocNew();
            if (pkt) {
              if (pkt->readFrom(_rx_buffer + 4, len)) {
                onPacketReceived(pkt);
              } else {
                BRIDGE_DEBUG_PRINTLN("RX failed to parse packet\n");
                _mgr->free(pkt);
              }
            } else {
              BRIDGE_DEBUG_PRINTLN("RX failed to allocate packet\n");
            }
          } else {
            BRIDGE_DEBUG_PRINTLN("RX checksum mismatch, rcv=0x%04x\n", received_checksum);
          }
          _rx_buffer_pos = 0; // Reset for next packet
        }
      }
    }
  }
}

void RS232Bridge::sendPacket(mesh::Packet *packet) {
  // Guard against uninitialized state
  if (_initialized == false) {
    return;
  }

  // First validate the packet pointer
  if (!packet) {
    BRIDGE_DEBUG_PRINTLN("TX invalid packet pointer\n");
    return;
  }

  if (!_seen_packets.wasSeen(packet)) {
    _seen_packets.markSeen(packet);

    uint8_t buffer[MAX_SERIAL_PACKET_SIZE];
    uint16_t len = packet->writeTo(buffer + 4);

    // Check if packet fits within our maximum payload size
    if (len > (MAX_TRANS_UNIT + 1)) {
      BRIDGE_DEBUG_PRINTLN("TX packet too large (payload=%d, max=%d)\n", len, MAX_TRANS_UNIT + 1);
      return;
    }

    // Build packet header
    buffer[0] = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF; // Magic high byte
    buffer[1] = BRIDGE_PACKET_MAGIC & 0xFF;        // Magic low byte
    buffer[2] = (len >> 8) & 0xFF;                 // Length high byte
    buffer[3] = len & 0xFF;                        // Length low byte

    // Calculate checksum over the payload
    uint16_t checksum = fletcher16(buffer + 4, len);
    buffer[4 + len] = (checksum >> 8) & 0xFF; // Checksum high byte
    buffer[5 + len] = checksum & 0xFF;        // Checksum low byte

    // Send complete packet
    RS232_BRIDGE_SERIAL.write(buffer, len + SERIAL_OVERHEAD);

    BRIDGE_DEBUG_PRINTLN("TX, len=%d crc=0x%04x\n", len, checksum);
  }
}

void RS232Bridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}
