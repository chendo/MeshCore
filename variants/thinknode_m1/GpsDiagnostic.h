#pragma once

/* A GPS diagnostic for the ThinkNode M1.
 *
 * The whole file is behind GPS_DIAGNOSTIC. A build that does not set the flag
 * gets an empty header, and the firmware does not change.
 *
 * The diagnostic prints a block at boot and a status line every 5 seconds. It
 * does not change the GPS logic, and it does not drive any GPS pin. It reads
 * pins, and it counts the bytes that come from the GPS module.
 *
 * The status line probes the switch pin, and the probe blocks the loop for
 * about 24 ms. That is 0.5 percent of the time. Use this build to find the
 * fault, and then go back to a normal build.
 *
 * Build it like this:
 *   pio run -e ThinkNode_M1_repeater_gps_diag
 * Then open the USB serial port at 115200 baud.
 */

#if GPS_DIAGNOSTIC

#include <Arduino.h>
#include <string.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>

#ifndef GPS_DIAG_PERIOD_MS
  #define GPS_DIAG_PERIOD_MS 5000
#endif

/* A Stream that sits between Serial1 and the NMEA parser.
 *
 * The parser gets every byte that the tap reads, so the tap does not change
 * what the GPS code does. The tap counts the bytes, and it keeps the last
 * complete sentence. Without the tap the parser eats the bytes, and the
 * diagnostic cannot tell an empty serial port from a busy one. */
class GpsSerialTap : public Stream {
  Stream* _src;
  uint32_t _bytes;        // bytes since the last status line
  uint32_t _total;        // bytes since boot
  uint32_t _sentences;    // complete sentences since boot
  uint8_t  _cur_len;
  char _cur[96];          // the sentence that is in progress
  char _last[96];         // the last complete sentence

public:
  GpsSerialTap(Stream& src)
    : _src(&src), _bytes(0), _total(0), _sentences(0), _cur_len(0) {
    _cur[0] = 0;
    _last[0] = 0;
  }

  // Stream and Print, all of it forwarded to the real port.
  int available() override { return _src->available(); }
  int peek() override { return _src->peek(); }
  void flush() override { _src->flush(); }
  size_t write(uint8_t b) override { return _src->write(b); }
  size_t write(const uint8_t* buf, size_t size) override { return _src->write(buf, size); }
  int availableForWrite() override { return _src->availableForWrite(); }

  int read() override {
    int c = _src->read();
    if (c < 0) return c;
    _bytes++;
    _total++;
    if (c == '\r' || c == '\n') {
      if (_cur_len > 0) {           // the sentence is complete
        memcpy(_last, _cur, _cur_len + 1);
        _sentences++;
      }
      _cur_len = 0;
      _cur[0] = 0;
    } else {
      if (c == '$') _cur_len = 0;   // a new sentence starts here
      if (_cur_len < (uint8_t)(sizeof(_cur) - 1)) _cur[_cur_len++] = (char) c;
      _cur[_cur_len] = 0;
    }
    return c;
  }

  // Read the counter and set it to zero.
  uint32_t takeBytes() { uint32_t n = _bytes; _bytes = 0; return n; }
  uint32_t totalBytes() const { return _total; }
  uint32_t sentenceCount() const { return _sentences; }
  const char* lastSentence() const { return _last[0] ? _last : NULL; }

  // Read and count the bytes that nobody wants. The GPS code reads the port
  // only when the GPS is active. If nobody reads the port, the receive buffer
  // fills, and the byte count stops. Then the diagnostic cannot see that bytes
  // still arrive.
  void drain() { while (_src->available()) read(); }
};

/* The result of a probe of one pin. */
struct GpsPinProbe {
  uint8_t no_pull;      // the level with no pull
  uint8_t pull_up;      // the level with the internal pull-up
  uint8_t pull_down;    // the level with the internal pull-down
  uint8_t high_count;   // how many of the no-pull samples were HIGH
  uint8_t samples;      // how many no-pull samples the probe took
};

/* Read one pin three ways, and put the pin back to INPUT.
 *
 * A pin that an external circuit holds gives the same level three times. A pin
 * that floats follows the pull that the probe applies. This one test tells the
 * two apart.
 *
 * The internal pull of the nRF52840 is about 13 kOhm. An external resistor that
 * is much weaker than that loses to the internal pull. So a floating result can
 * also mean a weak external resistor. Read the three levels, and do not read
 * the verdict only. */
static inline GpsPinProbe gps_diag_probe_pin(int pin, uint8_t samples) {
  GpsPinProbe p;
  p.samples = samples;

  pinMode(pin, INPUT);
  delay(5);
  p.high_count = 0;
  for (uint8_t i = 0; i < samples; i++) {
    if (digitalRead(pin) == HIGH) p.high_count++;
    delay(1);
  }
  p.no_pull = (p.high_count * 2 >= samples) ? 1 : 0;

  pinMode(pin, INPUT_PULLUP);
  delay(5);
  p.pull_up = digitalRead(pin) == HIGH ? 1 : 0;

  pinMode(pin, INPUT_PULLDOWN);
  delay(5);
  p.pull_down = digitalRead(pin) == HIGH ? 1 : 0;

  // Put the pin back the way the firmware wants it. See
  // ThinkNodeM1SensorManager::begin() in target.cpp.
  pinMode(pin, INPUT);
  delay(5);
  return p;
}

/* Say what the three levels mean. */
static inline const char* gps_diag_probe_verdict(const GpsPinProbe& p) {
  if (p.pull_up == 1 && p.pull_down == 0) {
    return "the pin follows the pull, so the pin FLOATS. No external circuit holds it, "
           "or the external resistor is much weaker than 13 kOhm";
  }
  if (p.pull_up == 1 && p.pull_down == 1) {
    return "an external circuit holds the pin HIGH. The pin does not float";
  }
  if (p.pull_up == 0 && p.pull_down == 0) {
    return "an external circuit holds the pin LOW. The pin does not float";
  }
  return "the pin reads against the pull. This is not normal. Look for a driver on the pin";
}

/* The block that the firmware prints one time at boot. */
static inline void gps_diag_boot_block(bool gps_active, bool switch_state_at_boot) {
  // Give the USB serial port time to come up. The owner must see this block on
  // the first try.
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);
  delay(200);

  GpsPinProbe sw = gps_diag_probe_pin(PIN_GPS_SWITCH, 16);
  int en = digitalRead(GPS_EN);
  int rst = digitalRead(PIN_GPS_REINIT);

  Serial.println();
  Serial.println("--- GPS DIAGNOSTIC: ThinkNode M1 ---");
  Serial.printf("pins: switch=P%d gps_en=P%d gps_reinit=P%d pwr_en=P%d uart_rx=P%d uart_tx=P%d\n",
                PIN_GPS_SWITCH, GPS_EN, PIN_GPS_REINIT, PIN_PWR_EN, PIN_SERIAL1_RX, PIN_SERIAL1_TX);
  Serial.printf("uart: our rx P%d is the GPS tx. our tx P%d is the GPS rx. Serial1 runs at 9600 baud.\n",
                PIN_SERIAL1_RX, PIN_SERIAL1_TX);
  Serial.printf("radio: SX126X_POWER_EN is P%d. It must stay HIGH.\n", SX126X_POWER_EN);
  Serial.printf("       initVariant() drives P%d HIGH, and no other code writes to it.\n",
                PIN_GPS_REINIT);
  Serial.printf("switch P%d probe: no-pull=%d (%d of %d samples HIGH)  pull-up=%d  pull-down=%d\n",
                PIN_GPS_SWITCH, sw.no_pull, sw.high_count, sw.samples, sw.pull_up, sw.pull_down);
  Serial.printf("switch P%d result: %s.\n", PIN_GPS_SWITCH, gps_diag_probe_verdict(sw));
  if (sw.high_count != 0 && sw.high_count != sw.samples) {
    Serial.printf("switch P%d note: with no pull the level is not steady. A floating pin does this.\n",
                  PIN_GPS_SWITCH);
  }
  Serial.printf("gps_en P%d: level=%d, active level is %s, so GPS power is %s.\n",
                GPS_EN, en, GPS_EN_ACTIVE == HIGH ? "HIGH" : "LOW",
                en == GPS_EN_ACTIVE ? "ON" : "OFF");
  Serial.printf("gps_reinit P%d: level=%d, so the GPS reset is %s. A LOW of 100 ms resets the module.\n",
                PIN_GPS_REINIT, rst, rst == LOW ? "ASSERTED" : "RELEASED");
  Serial.printf("pwr_en P%d: level=%d (initVariant drives it HIGH for the peripheral rail).\n",
                PIN_PWR_EN, digitalRead(PIN_PWR_EN));
  Serial.printf("boot: the code read the switch as %s, so start_gps() %s. gps_active=%d.\n",
                switch_state_at_boot ? "HIGH" : "LOW",
                switch_state_at_boot ? "ran" : "did not run",
                gps_active ? 1 : 0);
  if (sw.pull_up == 1 && sw.pull_down == 0) {
    Serial.printf("DIAGNOSIS: P%d has no pull. target.cpp sets it to INPUT with no pull, and the\n",
                  PIN_GPS_SWITCH);
    Serial.println("           board adds none. The pin reads noise, so the GPS switch does nothing.");
    Serial.println("           Fix: use INPUT_PULLUP or INPUT_PULLDOWN. The steps below show which one.");
  } else {
    Serial.printf("DIAGNOSIS: P%d is held, so the switch pin is not the fault in this position.\n",
                  PIN_GPS_SWITCH);
  }
  Serial.println();
  Serial.println("do this now:");
  Serial.println("  1. Move the GPS switch to the other position. Wait for 2 status lines.");
  Serial.println("     Compare up= and dn= in both positions. A position that holds the pin shows");
  Serial.println("     up=dn. A position that floats shows up=1 and dn=0.");
  Serial.println("  2. Read the status line like this:");
  Serial.println("     rx=0 and en=OFF -> the switch never turned the GPS on. The switch is the fault.");
  Serial.println("     rx=0 and en=ON  -> the module has power, but it sends nothing. Look at the");
  Serial.println("                        P41 and P40 wires, or the module is dead.");
  Serial.println("     rx>0 and fix=NO -> the module talks. Go outside, and wait for 15 minutes.");
  Serial.println("                        If there is still no fix, look at the antenna.");
  Serial.printf("a status line follows every %d ms.\n", GPS_DIAG_PERIOD_MS);
  Serial.println("--- END GPS DIAGNOSTIC ---");
  Serial.println();
}

/* The line that the firmware prints every GPS_DIAG_PERIOD_MS.
 *
 * Read it like this:
 *   rx=0 and en=OFF          -> the switch never turned the GPS on.
 *   rx=0 and en=ON           -> the module has power, but it sends nothing.
 *                               Look at the wires, or the module is dead.
 *   rx>0 and fix=NO          -> the module talks. It cannot see the sky, or
 *                               the antenna is bad. Wait 15 minutes outside.
 */
static inline void gps_diag_status_line(GpsSerialTap& tap, LocationProvider& loc, bool gps_active) {
  GpsPinProbe sw = gps_diag_probe_pin(PIN_GPS_SWITCH, 4);
  int en = digitalRead(GPS_EN);
  int rst = digitalRead(PIN_GPS_REINIT);
  const char* last = tap.lastSentence();

  Serial.printf("[gps] t=%lus sw=%d(up=%d,dn=%d) en=%d:%s reinit=%d:%s active=%d "
                "rx=%luB total=%luB nmea=%lu sats=%ld fix=%s last=%s\n",
                (unsigned long)(millis() / 1000),
                sw.no_pull, sw.pull_up, sw.pull_down,
                en, en == GPS_EN_ACTIVE ? "ON" : "OFF",
                rst, rst == LOW ? "ASSERTED" : "RELEASED",
                gps_active ? 1 : 0,
                (unsigned long) tap.takeBytes(),
                (unsigned long) tap.totalBytes(),
                (unsigned long) tap.sentenceCount(),
                loc.satellitesCount(),
                loc.isValid() ? "YES" : "NO",
                last ? last : "(none yet)");
}

#endif  // GPS_DIAGNOSTIC
