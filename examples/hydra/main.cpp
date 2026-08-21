#include <Arduino.h>   // PlatformIO needs this
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
  /* The watchdog limit for setup() is much larger than the limit for the
     running loop. A LittleFS format is slow. The creation of an identity is
     slow, and this code does it once for each slot. The SoftDevice role ladder
     is slow. All three are correctly slow. A reset in the middle of one of them
     would give a boot loop. */
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
  delay(2000);              // let the message go out, and slow down a boot loop
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
  /* A reset in the middle of a read leaves the slave device with SDA low. The
     TWIM driver in the core then waits for ever on EVENTS_STOPPED. It has no
     timeout. */
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
  // The colour shows the radio and the brightness shows the direction. Green
  // is LoRa. Dim is receive. Bright is transmit. Every slot shares the LED,
  // because there is one radio.
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
    char reply[160];   // the MyMesh CLI writes up to this size. Do not make it smaller.
    reply[0] = 0;
    hydra.handleCommand(0, command, reply, sizeof(reply));   // 0 = serial console
    if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
    command[0] = 0;
  }

#ifdef LOOP_WATCHDOG_MS
  LoopWatchdog::feed();
  /* The short runtime limit is safe only after the loop has shown that it
     runs. Before this point the boot limit covers setup(). */
  static bool wdog_tightened = false;
  if (!wdog_tightened) {
    wdog_tightened = true;
    LoopWatchdog::setLimit(LOOP_WATCHDOG_MS);
  }
#endif

  hydra.loop();          // every slot, then the arbiter. The order is important.

#if WITH_STATUS_LED
  status_led.loop();
#endif
  sensors.loop();
  rtc_clock.tick();

  // Powersave is a decision of the node. So the queue check must cover every
  // slot. The node must not sleep when only slot 0 is idle. That would stop the
  // sends of the chat slots. It would stop them for as long as the repeater had
  // nothing to say.
  if (hydra.repeater().prefs()->powersaving_enabled && !hydra.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0);   // the nRF ignores the seconds. It wakes on LoRa or a timer.
#else
    board.sleep(30);
#endif
  }
}
