#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>

ThinkNodeM1Board board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
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
#if ENV_INCLUDE_GPS == 1
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
#else
  // GPS compiled out. This has to be handled HERE, not just by the
  // ENV_INCLUDE_GPS guards in the companion example, because this manager
  // starts the receiver from the physical slide switch alone — it never
  // consults _prefs.gps_enabled. Leaving it stock would mean a build with
  // "GPS disabled" still powered and read the receiver whenever the switch
  // happened to be on, and still populated node_lat/node_lon.
  //
  // Hold the enable line low and leave the UART shut, so neither the switch
  // nor a settings command can bring it back.
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, LOW);
  gps_active = false;
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
#if ENV_INCLUDE_GPS != 1
  return;   // GPS compiled out: never poll the switch, never start the receiver
#else
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
#endif   // ENV_INCLUDE_GPS
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

