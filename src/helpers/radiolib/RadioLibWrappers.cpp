
#define RADIOLIB_STATIC_ONLY 1
#include "RadioLibWrappers.h"

#define STATE_IDLE       0
#define STATE_RX         1
#define STATE_TX_WAIT    3
#define STATE_TX_DONE    4
#define STATE_INT_READY 16

#define NUM_NOISE_FLOOR_SAMPLES  64
#define SAMPLING_THRESHOLD  14

static volatile uint8_t state = STATE_IDLE;

// The DIO1 interrupt used to be a bit inside `state`, set with `state |=
// STATE_INT_READY`. That is a read-modify-write racing against the plain
// stores the main loop makes to the same byte (state = STATE_RX, etc):
//
//   main: load state (IDLE) | ISR: state |= INT_READY | main: store STATE_RX
//
// and the interrupt is silently gone. A lost RX interrupt means a received
// packet is never read out — it sits in the FIFO until the next packet
// overwrites it, and it increments no counter, so the loss is invisible. A
// lost TX-done means isSendComplete() never becomes true.
//
// Split into a monotonic counter written ONLY by the ISR and a copy written
// ONLY by the main loop. Neither side updates the other's variable, so there
// is no shared read-modify-write and no update can be lost.
static volatile uint32_t irq_count = 0;   // ISR writes, main reads
static uint32_t irq_seen = 0;             // main writes only

static inline bool intReady() { return irq_count != irq_seen; }
static inline void clearInt() { irq_seen = irq_count; }

// this function is called when a complete packet
// is transmitted by the module
static
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  // we sent (or received) a packet, note the event
  irq_count++;
}

void RadioLibWrapper::begin() {
  _radio->setPacketReceivedAction(setFlag);  // this is also SentComplete interrupt
  _preamble_sf = getSpreadingFactor();
  _radio->setPreambleLength(preambleLengthForSF(_preamble_sf)); // longer preamble for lower SF improves reliability
  state = STATE_IDLE;
  clearInt();

  if (_board->getStartupReason() == BD_STARTUP_RX_PACKET) {  // received a LoRa packet (while in deep sleep)
    setFlag(); // LoRa packet is already received
  }

  _noise_floor = 0;
  _threshold = 0;
  _cad_enabled = false;

  // start average out some samples
  _num_floor_samples = 0;
  _floor_sample_sum = 0;
}

uint32_t RadioLibWrapper::getRngSeed() {
  return _radio->random(0x7FFFFFFF);
}

void RadioLibWrapper::setTxPower(int8_t dbm) {
  _radio->setOutputPower(dbm);
}

void RadioLibWrapper::idle() {
  if (state == STATE_TX_WAIT) { MESH_DEBUG_PRINTLN("DIAG: idle() while TX_WAIT!"); }
  _radio->standby();
  state = STATE_IDLE;   // need another startReceive()
}

void RadioLibWrapper::triggerNoiseFloorCalibrate(int threshold) {
  _threshold = threshold;
  if (_num_floor_samples >= NUM_NOISE_FLOOR_SAMPLES) {  // ignore trigger if currently sampling
    _num_floor_samples = 0;
    _floor_sample_sum = 0;
  }
}

void RadioLibWrapper::doResetAGC() {
  if (state == STATE_TX_WAIT) { MESH_DEBUG_PRINTLN("DIAG: doResetAGC() while TX_WAIT!"); }
  _radio->sleep();  // warm sleep to reset analog frontend
}

void RadioLibWrapper::resetAGC() {
  // make sure we're not mid-receive of packet!
  if (intReady() || isReceivingPacket()) return;

  doResetAGC();
  state = STATE_IDLE;   // trigger a startReceive()

  // Reset noise floor sampling so it reconverges from scratch.
  // Without this, a stuck _noise_floor of -120 makes the sampling threshold
  // too low (-106) to accept normal samples (~-105), self-reinforcing the
  // stuck value even after the receiver has recovered.
  _noise_floor = 0;
  _num_floor_samples = 0;
  _floor_sample_sum = 0;
}

void RadioLibWrapper::loop() {
  if (state == STATE_RX && _num_floor_samples < NUM_NOISE_FLOOR_SAMPLES) {
    if (!isReceivingPacket()) {
      int rssi = getCurrentRSSI();
      if (rssi < _noise_floor + SAMPLING_THRESHOLD) {  // only consider samples below current floor + sampling THRESHOLD
        _num_floor_samples++;
        _floor_sample_sum += rssi;
      }
    }
  } else if (_num_floor_samples >= NUM_NOISE_FLOOR_SAMPLES && _floor_sample_sum != 0) {
    _noise_floor = _floor_sample_sum / NUM_NOISE_FLOOR_SAMPLES;
    if (_noise_floor < -120) {
      _noise_floor = -120;    // clamp to lower bound of -120dBi
    }
    _floor_sample_sum = 0;

    MESH_DEBUG_PRINTLN("RadioLibWrapper: noise_floor = %d", (int)_noise_floor);
  }
}

void RadioLibWrapper::startRecv() {
  if (state == STATE_TX_WAIT) { MESH_DEBUG_PRINTLN("DIAG: startRecv() while TX_WAIT!"); }
  int err = _radio->startReceive();
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_RX;
  } else {
    MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
  }
}

bool RadioLibWrapper::isInRecvMode() const {
  return state == STATE_RX;
}

int RadioLibWrapper::recvRaw(uint8_t* bytes, int sz) {
  int len = 0;
  if (intReady()) {
    clearInt();
    len = _radio->getPacketLength();
    if (len > 0) {
      if (len > sz) { len = sz; }
      int err = _radio->readData(bytes, len);
      if (err != RADIOLIB_ERR_NONE) {
        MESH_DEBUG_PRINTLN("RadioLibWrapper: error: readData(%d)", err);
        // keep the damaged bytes before discarding the frame (see header)
        last_err_len = (uint8_t)(len > (int)sizeof(last_err_payload) ? sizeof(last_err_payload) : len);
        memcpy(last_err_payload, bytes, last_err_len);
        len = 0;
        n_recv_errors++;
        last_recv_error = (int16_t)err;
        // Split by cause: the mix is diagnostic. CRC-dominated points at
        // collisions (payload mangled but the frame still parsed); header
        // damage points at weak signal or interference corrupting the PHY
        // header itself, where even the length can't be trusted.
        switch (err) {
          case RADIOLIB_ERR_CRC_MISMATCH:   n_err_crc++;     break;
          case RADIOLIB_ERR_LORA_HEADER_DAMAGED: n_err_header++; break;
          case RADIOLIB_ERR_RX_TIMEOUT:     n_err_timeout++; break;
          default:                          n_err_other++;   break;
        }
      } else {
      //  Serial.print("  readData() -> "); Serial.println(len);
        n_recv++;
      }
    }
    state = STATE_IDLE;   // need another startReceive()
  }

  if (state != STATE_RX) {
    if (state == STATE_TX_WAIT) { MESH_DEBUG_PRINTLN("DIAG: recvRaw() re-arm while TX_WAIT!"); }
    int err = _radio->startReceive();
    if (err == RADIOLIB_ERR_NONE) {
      state = STATE_RX;
    } else {
      MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
    }
  }
  return len;
}

uint32_t RadioLibWrapper::getEstAirtimeFor(int len_bytes) {
  return _radio->getTimeOnAir(len_bytes) / 1000;
}

bool RadioLibWrapper::startSendRaw(const uint8_t* bytes, int len) {
  _board->onBeforeTransmit();
  int err = _radio->startTransmit((uint8_t *) bytes, len);
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_TX_WAIT;
    return true;
  }
  MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startTransmit(%d)", err);
  idle();   // trigger another startRecv()
  _board->onAfterTransmit();
  return false;
}

bool RadioLibWrapper::isSendComplete() {
  if (intReady()) {
    clearInt();
    state = STATE_IDLE;
    n_sent++;
    return true;
  }
  return false;
}

void RadioLibWrapper::onSendFinished() {
  _radio->finishTransmit();
  _board->onAfterTransmit();
  state = STATE_IDLE;
}

int16_t RadioLibWrapper::performChannelScan() {
  return _radio->scanChannel();
}

bool RadioLibWrapper::isChannelActive() {
  // int.thresh: RSSI-based interference detection (relative to noise floor)
  if (_threshold != 0 && getCurrentRSSI() > _noise_floor + _threshold) return true;

  // cad: hardware channel activity detection
  if (_cad_enabled) {
    int16_t result = performChannelScan();
    // scanChannel() triggers the DIO interrupt (CAD done) via setFlag().
    // Consume it before restarting RX so recvRaw() doesn't try to read a
    // non-existent packet and count a spurious recv error.
    clearInt();
    state = STATE_IDLE;
    startRecv();
    if (result != RADIOLIB_CHANNEL_FREE) return true;
  }

  return false;
}

float RadioLibWrapper::getLastRSSI() const {
  return _radio->getRSSI();
}
float RadioLibWrapper::getLastSNR() const {
  return _radio->getSNR();
}

// Approximate SNR threshold per SF for successful reception (based on Semtech datasheets)
static float snr_threshold[] = {
    -7.5,  // SF7 needs at least -7.5 dB SNR
    -10,   // SF8 needs at least -10 dB SNR
    -12.5, // SF9 needs at least -12.5 dB SNR
    -15,  // SF10 needs at least -15 dB SNR
    -17.5,// SF11 needs at least -17.5 dB SNR
    -20   // SF12 needs at least -20 dB SNR
};
  
float RadioLibWrapper::packetScoreInt(float snr, int sf, int packet_len) {
  if (sf < 7) return 0.0f;
  
  if (snr < snr_threshold[sf - 7]) return 0.0f;    // Below threshold, no chance of success

  auto success_rate_based_on_snr = (snr - snr_threshold[sf - 7]) / 10.0;
  auto collision_penalty = 1 - (packet_len / 256.0);   // Assuming max packet of 256 bytes

  return max(0.0, min(1.0, success_rate_based_on_snr * collision_penalty));
}
