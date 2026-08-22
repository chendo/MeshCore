#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include "GpsDiagnostic.h"   // empty unless GPS_DIAGNOSTIC is set

ThinkNodeM1Board board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
#if GPS_DIAGNOSTIC
  // The tap sits between Serial1 and the parser. It counts the bytes that come
  // from the GPS module. The parser still gets every byte. See GpsDiagnostic.h.
  GpsSerialTap gps_tap(Serial1);
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(gps_tap, &rtc_clock);
#else
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
#endif
ThinkNodeM1SensorManager sensors = ThinkNodeM1SensorManager(nmea);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  rtc_clock.begin(Wire);
  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

void ThinkNodeM1SensorManager::start_gps() {
  if (!gps_active) {
    gps_active = true;
    _location->begin();
  }
}

void ThinkNodeM1SensorManager::stop_gps() {
  if (gps_active) {
    gps_active = false;
    _location->stop();
  }
}

bool ThinkNodeM1SensorManager::begin() {
  Serial1.begin(9600);

  // Initialize GPS switch pin
  pinMode(PIN_GPS_SWITCH, INPUT);
  last_gps_switch_state = digitalRead(PIN_GPS_SWITCH);

  // Initialize GPS power pin
  pinMode(GPS_EN, OUTPUT);

  // Check initial switch state to determine if GPS should be active
  if (last_gps_switch_state == HIGH) {  // Switch is HIGH when ON
    start_gps();
  }

#if GPS_DIAGNOSTIC
  // The probe reads PIN_GPS_SWITCH three ways, and it puts the pin back to
  // INPUT. It does not touch last_gps_switch_state, so the switch logic above
  // keeps its own record.
  gps_diag_boot_block(gps_active, last_gps_switch_state);
#endif

  return true;
}

bool ThinkNodeM1SensorManager::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  if (requester_permissions & TELEM_PERM_LOCATION) {   // does requester have permission?
    telemetry.addGPS(TELEM_CHANNEL_SELF, node_lat, node_lon, node_altitude);
  }
  return true;
}

void ThinkNodeM1SensorManager::loop() {
  static long next_gps_update = 0;
  static long last_switch_check = 0;

  // Check GPS switch state every second
  if (millis() - last_switch_check > 1000) {
    bool current_switch_state = digitalRead(PIN_GPS_SWITCH);
    
    // Detect switch state change
    if (current_switch_state != last_gps_switch_state) {
      last_gps_switch_state = current_switch_state;
      
      if (current_switch_state == HIGH) {  // Switch is ON
        MESH_DEBUG_PRINTLN("GPS switch ON");
        start_gps();
      } else {  // Switch is OFF
        MESH_DEBUG_PRINTLN("GPS switch OFF");
        stop_gps();
      }
    }
    
    last_switch_check = millis();
  }

#if GPS_DIAGNOSTIC
  {
    static unsigned long next_gps_diag = GPS_DIAG_PERIOD_MS;

    // The GPS code reads Serial1 only when the GPS is active. When it is not
    // active, the receive buffer fills, and the byte count stops. Read the
    // bytes here so that the count stays true.
    if (!gps_active) gps_tap.drain();

    if ((long)(millis() - next_gps_diag) >= 0) {
      gps_diag_status_line(gps_tap, *_location, gps_active);
      next_gps_diag = millis() + GPS_DIAG_PERIOD_MS;
    }
  }
#endif

  if (!gps_active) {
    return;  // GPS is not active, skip further processing
  }

  _location->loop();

  if (millis() > next_gps_update) {
    if (_location->isValid()) {
      node_lat = ((double)_location->getLatitude())/1000000.;
      node_lon = ((double)_location->getLongitude())/1000000.;
      node_altitude = ((double)_location->getAltitude()) / 1000.0;
      MESH_DEBUG_PRINTLN("lat %f lon %f", node_lat, node_lon);
    }
    next_gps_update = millis() + 1000;
  }
}

int ThinkNodeM1SensorManager::getNumSettings() const {
  return 1;  // always show GPS setting
}

const char* ThinkNodeM1SensorManager::getSettingName(int i) const {
  return (i == 0) ? "gps" : NULL;
}

const char* ThinkNodeM1SensorManager::getSettingValue(int i) const {
  if (i == 0) {
    return gps_active ? "1" : "0";
  }
  return NULL;
}

bool ThinkNodeM1SensorManager::setSettingValue(const char* name, const char* value) {
  if (strcmp(name, "gps") == 0) {
    if (strcmp(value, "0") == 0) {
      stop_gps();
    } else {
      start_gps();
    }
    return true;
  }
  return false;  // not supported
}

