#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "HydraNode.h"

#if WITH_STATUS_LED
  #include <helpers/StatusLed.h>
  static StatusLed status_led;
#endif
#ifdef NRF52_PLATFORM
  #include <helpers/nrf52/I2CBusRecovery.h>
#endif
#ifdef LOOP_WATCHDOG_MS
  #include <helpers/nrf52/LoopWatchdog.h>
  /* setup() is watched far more loosely than the running loop: a LittleFS
     format, identity generation (once per slot here) or the SoftDevice role
     ladder are all legitimately slow, and resetting partway through one would
     boot-loop. */
  #define BOOT_WATCHDOG_MS 120000
  #define WDOG_FEED() LoopWatchdog::feed()
#else
  #define WDOG_FEED() do {} while (0)
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

static char command[160];

static void halt() {
  Serial.println("HALT: radio init failed, rebooting");
  Serial.flush();
  delay(2000);              // let the message out, and rate-limit a boot loop
  board.reboot();
  while (1) ;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

#ifdef LOOP_WATCHDOG_MS
  LoopWatchdog::begin(BOOT_WATCHDOG_MS);
#endif

  board.begin();
  WDOG_FEED();

#if defined(NRF52_PLATFORM) && defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  /* A reset partway through a read leaves the slave holding SDA low, and the
     core's TWIM driver then spins forever on EVENTS_STOPPED with no timeout. */
  if (!I2CBusRecovery::recover(PIN_WIRE_SDA, PIN_WIRE_SCL)) {
    Serial.println("I2C: bus stuck, recovery failed");
  }
#endif

  if (!radio_init()) halt();
  WDOG_FEED();

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
#else
  #error "need to define filesystem"
#endif

  sensors.begin();
  WDOG_FEED();

  hydra.begin(fs);
  WDOG_FEED();

#if WITH_STATUS_LED
  // Colour is the radio, brightness is the direction: green = LoRa, dim =
  // receive, bright = transmit. Shared across every slot — there is one radio.
  status_led.begin(LED_BLUE, LED_GREEN, LED_STATE_ON);
#endif

  command[0] = 0;
  board.onBootComplete();
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < (int)sizeof(command) - 1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command) - 1) command[sizeof(command) - 1] = '\r';

  if (len > 0 && command[len - 1] == '\r') {
    Serial.print('\n');
    command[len - 1] = 0;
    char reply[160];   // MyMesh's CLI writes up to this; do not shrink
    reply[0] = 0;
    hydra.handleCommand(command, reply, sizeof(reply));
    if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
    command[0] = 0;
  }

#ifdef LOOP_WATCHDOG_MS
  LoopWatchdog::feed();
  /* The tight runtime limit is only safe once the loop has proved it runs;
     until here the boot limit covers setup(). */
  static bool wdog_tightened = false;
  if (!wdog_tightened) {
    wdog_tightened = true;
    LoopWatchdog::setLimit(LOOP_WATCHDOG_MS);
  }
#endif

  hydra.loop();          // every slot, then the arbiter — order matters

#if WITH_STATUS_LED
  status_led.loop();
#endif
  sensors.loop();
  rtc_clock.tick();
}
