#include <Arduino.h>   // PlatformIO needs this
#include <Mesh.h>

#include "HydraNode.h"

#if WITH_STATUS_LED
  #include <helpers/StatusLed.h>
  static StatusLed status_led;
#endif
/* WiFi is a property of the board, not of the build. Every ESP32 part hydra
   runs on has a radio, so the console is compiled in wherever the hardware can
   carry it, and starts itself when an SSID has been configured. The nRF52
   boards have no WiFi, so this whole block vanishes there. */
#if defined(ESP32)
  #define WITH_WIFI_CONSOLE 1
#endif

#ifdef WITH_WIFI_CONSOLE
  #include <helpers/esp32/WifiConsole.h>
  static WifiConsole wifi_console;
  /* Not 0. CommonCLI reads sender_timestamp == 0 as "typed on the attached
     cable" and only then exports a private key. A LAN socket has not earned
     that, so TCP input is tagged as remote. */
  #define WIFI_CONSOLE_SENDER 1
#endif

/* THE BLE COMPANION FACADE. It makes this node answerable by the MeshCore
   phone app, so that an operator beside the board can administer every
   identity on it and spend no LoRa airtime. The same link carries the text
   CLI. See examples/hydra/HydraCompanion.h.

   A build that turns this on must NOT also build the BLE bridge. There is one
   BLE stack on the board, and the bridge owns it. */
#ifdef WITH_COMPANION_BLE
  /* WHICH stack carries it is a separate choice from WHETHER to carry it, so
     it is made the way this tree already selects radios, displays and bridges:
     a *_CLASS macro plus its header, defaulting per platform. On ESP32 that
     default is Bluedroid, because it is what every existing build links; an
     env opts into NimBLE by defining both macros. The two stacks cannot share
     a binary, so this is a choice and not a fallback chain. */
  #ifndef BLE_SERIAL_CLASS
    #ifdef NRF52_PLATFORM
      #define BLE_SERIAL_CLASS  SerialBLEInterface
      #define BLE_SERIAL_HEADER "helpers/nrf52/SerialBLEInterface.h"
    #elif defined(ESP32)
      #define BLE_SERIAL_CLASS  SerialBLEInterface
      #define BLE_SERIAL_HEADER "helpers/esp32/SerialBLEInterface.h"
    #else
      #error "WITH_COMPANION_BLE needs a SerialBLEInterface for this platform"
    #endif
  #endif
  #include BLE_SERIAL_HEADER
  #include "HydraCompanion.h"
  #ifndef BLE_NAME_PREFIX
    #define BLE_NAME_PREFIX "MeshCore-"
  #endif
  static BLE_SERIAL_CLASS       companion_ble;
  static HydraCompanionHost     companion_host;
  static companion::CompanionFacade companion_facade(companion_ble, companion_host);
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

#include <helpers/AppModule.h>

/* The node's modules, as modules. Each is a thin adapter over something that
   already existed: the point is not to rewrite them but to stop main.cpp being
   the only place that knows they exist. A module added later registers itself
   and needs no edit here.
   All NODE scope -- an LED, a sensor bus, a console and a BLE link belong to
   the board, not to any one identity. Nothing here wants a packet. */
#if WITH_STATUS_LED
class StatusLedModule : public AppModule {
public:
  AppScope scope() const override { return AppScope::NODE; }
  void onSetup() override { status_led.begin(LED_BLUE, LED_GREEN, LED_STATE_ON); }
  void onLoop() override { status_led.loop(); }
};
static StatusLedModule status_led_module;
#endif

class SensorsModule : public AppModule {
public:
  AppScope scope() const override { return AppScope::NODE; }
  void onSetup() override { sensors.begin(); }
  void onLoop() override { sensors.loop(); }
};
static SensorsModule sensors_module;

#ifdef WITH_WIFI_CONSOLE
/* onLoop() only pumps the transport. Reading a line stays in the CLI block
   below, because a console line has to reach dispatch(), and dispatch() is the
   entrypoint's business and not a module's. loopAll() runs before that block,
   so the ordering the console needs is preserved. */
class WifiConsoleModule : public AppModule {
public:
  AppScope scope() const override { return AppScope::NODE; }
  void onSetup() override {
  #ifdef WIFI_NTP_SERVER
    wifi_console.enableNtp(WIFI_NTP_SERVER);
  #endif
    wifi_console.begin();
  }
  void onLoop() override { wifi_console.loop(); }
};
static WifiConsoleModule wifi_console_module;
#endif

#ifdef WITH_COMPANION_BLE
class CompanionModule : public AppModule {
public:
  AppScope scope() const override { return AppScope::NODE; }
  void onSetup() override {
    // The name of slot 0 is the BLE device name. begin() may rewrite the buffer
    // when the name is "@@MAC", which is why it takes a writable string.
    companion_ble.begin(BLE_NAME_PREFIX, hydra.repeater().prefs()->node_name, BLE_PIN_CODE);
    companion_ble.enable();
  }
  void onLoop() override { companion_facade.loop(); }
  bool hasPendingWork() const override { return companion_ble.isConnected(); }
};
static CompanionModule companion_module;
#endif

static void registerModules() {
#if WITH_STATUS_LED
  AppModules::add(&status_led_module);
#endif
  AppModules::add(&sensors_module);
#ifdef WITH_WIFI_CONSOLE
  AppModules::add(&wifi_console_module);
#endif
#ifdef WITH_COMPANION_BLE
  AppModules::add(&companion_module);
#endif
}

static char command[160];

/* Both consoles funnel through here. WiFi is owned by main.cpp, not HydraNode:
   the node has no business knowing about a transport that only exists on one
   of the three chip families it runs on. */
static void dispatch(uint32_t sender, char* cmd, char* reply, size_t reply_sz) {
#ifdef WITH_WIFI_CONSOLE
  if (memcmp(cmd, "set wifi.ssid ", 14) == 0) {
    wifi_console.setCredentials(&cmd[14], "");   // ssid change clears the key
    StrHelper::strncpy(reply, "OK - set wifi.pwd next", reply_sz);
    return;
  }
  if (memcmp(cmd, "set wifi.pwd ", 13) == 0) {
    // rest of line verbatim: a passphrase may contain spaces
    wifi_console.setCredentials(wifi_console.ssid(), &cmd[13]);
    StrHelper::strncpy(reply, "OK - joining", reply_sz);
    return;
  }
  if (strcmp(cmd, "get wifi") == 0 || strcmp(cmd, "wifi") == 0) {
    wifi_console.status(reply, reply_sz);
    return;
  }
  if (strcmp(cmd, "wifi off") == 0) {
    wifi_console.setCredentials("", "");
    StrHelper::strncpy(reply, "OK - wifi off and forgotten", reply_sz);
    return;
  }
#endif
  hydra.handleCommand(sender, cmd, reply, reply_sz);
}

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

  WDOG_FEED();

  hydra.begin(fs);
  WDOG_FEED();



  command[0] = 0;
  registerModules();
  AppModules::setupAll();
  board.onBootComplete();
}

void loop() {
  AppModules::loopAll();

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
    dispatch(0, command, reply, sizeof(reply));   // 0 = serial console
    if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
    command[0] = 0;
  }

#ifdef WITH_WIFI_CONSOLE
  if (const char* line = wifi_console.takeLine()) {
    char wreply[160];
    wreply[0] = 0;
    dispatch(WIFI_CONSOLE_SENDER, (char*)line, wreply, sizeof(wreply));
    if (wreply[0]) wifi_console.reply(wreply);
  }
#endif

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


  rtc_clock.tick();

  // Powersave is a decision of the node. So the queue check must cover every
  // slot. The node must not sleep when only slot 0 is idle. That would stop the
  // sends of the chat slots. It would stop them for as long as the repeater had
  // nothing to say.
  // A node that sleeps while a phone is attached drops the connection.
#ifdef WITH_COMPANION_BLE
  if (companion_ble.isConnected()) return;
#endif
  if (hydra.repeater().prefs()->powersaving_enabled && !hydra.hasPendingWork()
      && !AppModules::anyPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0);   // the nRF ignores the seconds. It wakes on LoRa or a timer.
#else
    board.sleep(30);
#endif
  }
}
