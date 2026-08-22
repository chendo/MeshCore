#pragma once

/* A GPS diagnostic for the ThinkNode M1.
 *
 * The switch-pin logic at the top of the file is pure. It has no Arduino, and
 * test/test_gps_switch builds it on the native env. Everything below
 * GPS_DIAGNOSTIC needs Arduino. A build that does not set the flag compiles
 * only the inline functions, references none of them, and does not change.
 *
 * The diagnostic prints a block at boot and a status line every 5 seconds. It
 * does not change the GPS logic, and it does not drive any GPS pin. It reads
 * pins, and it counts the bytes that come from the GPS module.
 *
 * The status line probes the switch pin, and the probe blocks the loop for
 * about 34 ms. That is 0.7 percent of the time. Use this build to measure, and
 * then go back to a normal build.
 *
 * Build it like this:
 *   pio run -e ThinkNode_M1_repeater_gps_diag
 * Then open the USB serial port at 115200 baud.
 */

#include <stdint.h>

/* The result of a probe of one pin. */
struct GpsPinProbe {
  uint8_t no_pull;      // the level with no pull
  uint8_t pull_up;      // the level with the internal pull-up
  uint8_t pull_down;    // the level with the internal pull-down
  uint8_t recover;      // the level after the opposite pull is applied and then released
  uint8_t high_count;   // how many of the no-pull samples were HIGH
  uint8_t samples;      // how many no-pull samples the probe took
};

/* How hard the pin holds its level against the two internal pulls.
 *
 * The internal pull of the nRF52840 is about 13 kOhm. A drive that is weaker
 * than that loses to the internal pull and reads the same as an open pin, so
 * this text never says "floating". The recovery read below tells those apart. */
static inline const char* gps_diag_probe_observation(const GpsPinProbe& p) {
  if (p.pull_up == 1 && p.pull_down == 0) {
    return "the pin followed both internal pulls, so nothing drives it harder than the "
           "internal 13 kOhm";
  }
  if (p.pull_up == 1 && p.pull_down == 1) {
    return "the pin stayed HIGH against both internal pulls, so something drives it HIGH "
           "harder than 13 kOhm";
  }
  if (p.pull_up == 0 && p.pull_down == 0) {
    return "the pin stayed LOW against both internal pulls, so something drives it LOW "
           "harder than 13 kOhm";
  }
  return "the pin read against each internal pull. Noise between the two reads does this, "
         "and so does a driver that fights the pull";
}

/* True when the pin went back to its own level after the opposite internal pull
 * was applied and released.
 *
 * An open pin keeps the charge that the pull left on it. Pin leakage is in the
 * nanoamps, so the level stays put for many milliseconds. Any real drive, even
 * a 1 MOhm one, puts the pin back in microseconds. This is what separates an
 * open pin from a weakly driven one in ONE switch position. */
static inline bool gps_diag_probe_recovered(const GpsPinProbe& p) {
  return p.recover == p.no_pull;
}

static inline const char* gps_diag_recovery_observation(const GpsPinProbe& p) {
  if (gps_diag_probe_recovered(p)) {
    return "the pin went back to its own level after the opposite pull was released, so "
           "something drives it here";
  }
  return "the pin kept the level that the pull left on it, so nothing drives it here";
}

/* Every probe since boot, folded together.
 *
 * The bits in the masks are indexed by the plain-INPUT level: bit 0 for LOW,
 * bit 1 for HIGH. The switch position is not visible to the firmware, so the
 * plain-INPUT level is the only handle the probe has on it. */
struct GpsSwitchHistory {
  uint16_t probes;
  uint8_t levels_seen;    // the plain-INPUT levels the probe has read
  uint8_t pull_wins;      // at that level, an internal pull changed the reading
  uint8_t drive_wins;     // at that level, the pin held out against both pulls
  uint8_t restored;       // at that level, the pin recovered after the opposite pull
  uint8_t held_forced;    // at that level, the pin kept the level the pull left
  uint8_t unsteady;       // at that level, the no-pull samples disagreed
  uint8_t against_pull;   // some probe read pull_up=0 with pull_down=1
};

static inline void gps_diag_history_reset(GpsSwitchHistory& h) {
  h.probes = 0;
  h.levels_seen = 0;
  h.pull_wins = 0;
  h.drive_wins = 0;
  h.restored = 0;
  h.held_forced = 0;
  h.unsteady = 0;
  h.against_pull = 0;
}

static inline void gps_diag_history_add(GpsSwitchHistory& h, const GpsPinProbe& p) {
  uint8_t bit = p.no_pull ? 0x02 : 0x01;
  h.probes++;
  h.levels_seen |= bit;
  if (p.pull_up != p.pull_down) h.pull_wins |= bit; else h.drive_wins |= bit;
  if (gps_diag_probe_recovered(p)) h.restored |= bit; else h.held_forced |= bit;
  if (p.high_count != 0 && p.high_count != p.samples) h.unsteady |= bit;
  if (p.pull_up == 0 && p.pull_down == 1) h.against_pull = 1;
}

enum GpsSwitchFinding {
  GPS_SW_NO_DATA = 0,
  GPS_SW_ONE_LEVEL_DRIVEN,       // one plain level so far, and something drives the pin
  GPS_SW_ONE_LEVEL_OPEN,         // one plain level so far, and nothing drives the pin
  GPS_SW_ONE_LEVEL_MIXED,        // one plain level so far, and the probes disagree
  GPS_SW_BOTH_LEVELS_UNSTEADY,   // both plain levels, but the samples were not steady
  GPS_SW_FOLLOWS_SWITCH          // both plain levels, each of them steady
};

static inline GpsSwitchFinding gps_diag_switch_finding(const GpsSwitchHistory& h) {
  if (h.probes == 0) return GPS_SW_NO_DATA;
  if (h.levels_seen == 0x03) {
    // An open pin drifts between the two levels on its own. Only a steady level
    // in each position shows a switch.
    return h.unsteady ? GPS_SW_BOTH_LEVELS_UNSTEADY : GPS_SW_FOLLOWS_SWITCH;
  }
  uint8_t driven = (uint8_t)((h.drive_wins | h.restored) & h.levels_seen);
  uint8_t open = (uint8_t)(h.held_forced & h.levels_seen);
  if (driven && open) return GPS_SW_ONE_LEVEL_MIXED;
  if (open) return GPS_SW_ONE_LEVEL_OPEN;
  return GPS_SW_ONE_LEVEL_DRIVEN;
}

/* Only one finding is a conclusion. The rest need the other switch position,
 * and the firmware cannot ask for it. */
static inline bool gps_diag_finding_is_conclusive(GpsSwitchFinding f) {
  return f == GPS_SW_FOLLOWS_SWITCH;
}

static inline const char* gps_diag_finding_text(GpsSwitchFinding f) {
  switch (f) {
    case GPS_SW_FOLLOWS_SWITCH:
      return "the plain-INPUT level changed, and it was steady in each position. The "
             "switch drives the pin, and the plain INPUT that target.cpp uses reads it "
             "correctly. Change nothing";
    case GPS_SW_BOTH_LEVELS_UNSTEADY:
      return "both plain-INPUT levels have come up, but the samples were not steady in at "
             "least one of them. An open pin drifts like that, so this is not proof that "
             "the switch drives the pin";
    case GPS_SW_ONE_LEVEL_DRIVEN:
      return "the plain-INPUT level has not changed yet, and something drives the pin to "
             "it. The pin is not open in this position";
    case GPS_SW_ONE_LEVEL_OPEN:
      return "the plain-INPUT level has not changed yet, and nothing drives the pin. It is "
             "open in this position, which is normal if this position disconnects it";
    case GPS_SW_ONE_LEVEL_MIXED:
      return "the plain-INPUT level has not changed yet, and the probes disagree on "
             "whether anything drives the pin";
    default:
      return "no probe has run yet";
  }
}

/* What the operator has to do to turn the finding into a conclusion. */
static inline const char* gps_diag_finding_next_step(GpsSwitchFinding f) {
  if (f == GPS_SW_FOLLOWS_SWITCH) return "nothing. The measurement is complete";
  if (f == GPS_SW_NO_DATA) return "wait for the first probe";
  if (f == GPS_SW_BOTH_LEVELS_UNSTEADY) {
    return "hold the switch still, and watch sw= over several status lines. A switch gives "
           "a steady level in each position";
  }
  return "move the GPS switch to the other position, and wait for 2 status lines. If sw= "
         "never changes there, the switch does not reach this pin";
}

/* True when some probe found that an internal pull beats whatever drives the
 * pin. Then an internal pull would hold the pin at one level for good. */
static inline bool gps_diag_pull_would_override(const GpsSwitchHistory& h) {
  return h.pull_wins != 0;
}

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

/* Every probe since boot. The boot block and the status line share it. */
static GpsSwitchHistory gps_diag_history = { 0, 0, 0, 0, 0, 0, 0, 0 };
static GpsSwitchFinding gps_diag_last_finding = GPS_SW_NO_DATA;

/* Read one pin four ways, and leave the pin as a plain INPUT. */
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

  // Force the pin away from its own level, release, and look again. See
  // gps_diag_probe_recovered() for what the answer means.
  pinMode(pin, p.no_pull ? INPUT_PULLDOWN : INPUT_PULLUP);
  delay(5);
  pinMode(pin, INPUT);
  delay(5);
  p.recover = digitalRead(pin) == HIGH ? 1 : 0;

  // Leave the pin the way the firmware wants it. See
  // ThinkNodeM1SensorManager::begin() in target.cpp.
  pinMode(pin, INPUT);
  delay(5);
  return p;
}

// "L", "H", or "LH" for the plain-INPUT levels seen since boot.
static inline const char* gps_diag_levels_text(uint8_t levels_seen) {
  switch (levels_seen) {
    case 0x01: return "L";
    case 0x02: return "H";
    case 0x03: return "LH";
    default:   return "-";
  }
}

/* Print the standing rule about internal pulls. The probe never advises one. */
static inline void gps_diag_print_pull_rule() {
  Serial.printf("NOTE: an internal pull changed the reading of P%d. INPUT_PULLUP or\n",
                PIN_GPS_SWITCH);
  Serial.println("      INPUT_PULLDOWN would hold the pin at one level and stop the switch");
  Serial.println("      from working. target.cpp uses plain INPUT. Keep it that way.");
}

/* The block that the firmware prints one time at boot. */
static inline void gps_diag_boot_block(bool gps_active, bool switch_state_at_boot) {
  // Give the USB serial port time to come up. The owner must see this block on
  // the first try.
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);
  delay(200);

  GpsPinProbe sw = gps_diag_probe_pin(PIN_GPS_SWITCH, 16);
  gps_diag_history_add(gps_diag_history, sw);
  GpsSwitchFinding finding = gps_diag_switch_finding(gps_diag_history);
  gps_diag_last_finding = finding;
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
  Serial.println();
  Serial.println("OBSERVED, this position only:");
  Serial.printf("  switch P%d: no-pull=%d (%d of %d samples HIGH)  pull-up=%d  pull-down=%d\n",
                PIN_GPS_SWITCH, sw.no_pull, sw.high_count, sw.samples, sw.pull_up, sw.pull_down);
  Serial.printf("  strength: %s.\n", gps_diag_probe_observation(sw));
  Serial.printf("  recovery: %s (it read back %d).\n",
                gps_diag_recovery_observation(sw), sw.recover);
  if (sw.high_count != 0 && sw.high_count != sw.samples) {
    Serial.println("  the no-pull level was not steady. An open pin does this, and so does a");
    Serial.println("  weak drive with noise on it.");
  }
  Serial.printf("  gps_en P%d: level=%d, active level is %s, so GPS power is %s.\n",
                GPS_EN, en, GPS_EN_ACTIVE == HIGH ? "HIGH" : "LOW",
                en == GPS_EN_ACTIVE ? "ON" : "OFF");
  Serial.printf("  gps_reinit P%d: level=%d, so the GPS reset is %s. A LOW of 100 ms resets the module.\n",
                PIN_GPS_REINIT, rst, rst == LOW ? "ASSERTED" : "RELEASED");
  Serial.printf("  pwr_en P%d: level=%d (initVariant drives it HIGH for the peripheral rail).\n",
                PIN_PWR_EN, digitalRead(PIN_PWR_EN));
  Serial.printf("  boot: the code read the switch as %s, so start_gps() %s. gps_active=%d.\n",
                switch_state_at_boot ? "HIGH" : "LOW",
                switch_state_at_boot ? "ran" : "did not run",
                gps_active ? 1 : 0);

  Serial.println();
  Serial.printf("FINDING (levels seen so far: %s): %s.\n",
                gps_diag_levels_text(gps_diag_history.levels_seen),
                gps_diag_finding_text(finding));
  if (!gps_diag_finding_is_conclusive(finding)) {
    Serial.printf("NO DIAGNOSIS YET. One switch position does not show whether the switch\n");
    Serial.printf("changes P%d. Both positions are needed for that.\n", PIN_GPS_SWITCH);
  }
  Serial.printf("next: %s.\n", gps_diag_finding_next_step(finding));
  if (gps_diag_pull_would_override(gps_diag_history)) gps_diag_print_pull_rule();

  Serial.println();
  Serial.println("do this now:");
  Serial.println("  1. Move the GPS switch to the other position. Wait for 2 status lines.");
  Serial.println("     Watch sw= in the status line. That is the plain-INPUT level, and it is");
  Serial.println("     the level that target.cpp reads.");
  Serial.println("     sw= changes, and it is steady in each position -> the switch works.");
  Serial.println("     sw= never changes, and rec= equals sw=        -> something drives the pin,");
  Serial.println("                                                      but it is not the switch.");
  Serial.println("     sw= never changes, and rec= differs from sw=  -> the pin is open here.");
  Serial.println("     sw= drifts on its own                         -> the pin is open. That is");
  Serial.println("                                                      noise, not the switch.");
  Serial.println("  2. Read the rest of the status line like this:");
  Serial.println("     rx=0 and en=OFF -> the firmware has not turned the GPS on. That is correct");
  Serial.println("                        when the switch is OFF. Move the switch and look again.");
  Serial.println("     rx=0 and en=ON  -> the module has power and sends nothing. Check rst=, the");
  Serial.printf("                        P%d and P%d wires, and the 9600 baud rate.\n",
                PIN_SERIAL1_RX, PIN_SERIAL1_TX);
  Serial.println("     rx>0 and fix=NO -> the module talks. Indoors, sats=0 and fix=NO are normal.");
  Serial.println("                        Go outside for a fix. Only then look at the antenna.");
  Serial.printf("a status line follows every %d ms.\n", GPS_DIAG_PERIOD_MS);
  Serial.println("--- END GPS DIAGNOSTIC ---");
  Serial.println();
}

/* The line that the firmware prints every GPS_DIAG_PERIOD_MS.
 *
 * sw= is the plain-INPUT level, which is what target.cpp reads. up= and dn=
 * are the same pin with an internal pull applied, and they say how hard the
 * pin is driven. rec= is the level after the opposite pull was released:
 * rec=sw means something drives the pin, rec!=sw means nothing does. seen=
 * lists every sw= level since boot. A FINDING line follows whenever the
 * evidence changes, and not on every probe.
 *
 *   rx=0 and en=OFF -> the firmware has not turned the GPS on. Correct when
 *                      the switch is OFF.
 *   rx=0 and en=ON  -> the module has power and sends nothing.
 *   rx>0 and fix=NO -> the module talks. Indoors this is normal.
 */
static inline void gps_diag_status_line(GpsSerialTap& tap, LocationProvider& loc, bool gps_active) {
  GpsPinProbe sw = gps_diag_probe_pin(PIN_GPS_SWITCH, 4);
  gps_diag_history_add(gps_diag_history, sw);
  int en = digitalRead(GPS_EN);
  int rst = digitalRead(PIN_GPS_REINIT);
  const char* last = tap.lastSentence();

  Serial.printf("[gps] t=%lus sw=%d(up=%d,dn=%d,rec=%d) seen=%s en=%d:%s reinit=%d:%s active=%d "
                "rx=%luB total=%luB nmea=%lu sats=%ld fix=%s last=%s\n",
                (unsigned long)(millis() / 1000),
                sw.no_pull, sw.pull_up, sw.pull_down, sw.recover,
                gps_diag_levels_text(gps_diag_history.levels_seen),
                en, en == GPS_EN_ACTIVE ? "ON" : "OFF",
                rst, rst == LOW ? "ASSERTED" : "RELEASED",
                gps_active ? 1 : 0,
                (unsigned long) tap.takeBytes(),
                (unsigned long) tap.totalBytes(),
                (unsigned long) tap.sentenceCount(),
                loc.satellitesCount(),
                loc.isValid() ? "YES" : "NO",
                last ? last : "(none yet)");

  GpsSwitchFinding finding = gps_diag_switch_finding(gps_diag_history);
  if (finding == gps_diag_last_finding) return;   // one line per change, not per probe
  gps_diag_last_finding = finding;

  if (gps_diag_finding_is_conclusive(finding)) {
    Serial.printf("[gps] DIAGNOSIS: %s.\n", gps_diag_finding_text(finding));
  } else {
    Serial.printf("[gps] FINDING: %s.\n", gps_diag_finding_text(finding));
    Serial.printf("[gps] no diagnosis yet. Next: %s.\n", gps_diag_finding_next_step(finding));
  }
  if (gps_diag_pull_would_override(gps_diag_history)) gps_diag_print_pull_rule();
}

#endif  // GPS_DIAGNOSTIC
