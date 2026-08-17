#include <Arduino.h>   // needed for PlatformIO
#if WITH_BLE_CLI
  #include <helpers/nrf52/SerialBLEInterface.h>
#endif
#include <Mesh.h>

#include "MyMesh.h"
#ifdef NRF52_PLATFORM
  #include <helpers/nrf52/I2CBusRecovery.h>
#endif
#ifdef LOOP_WATCHDOG_MS
  #include <helpers/nrf52/LoopWatchdog.h>
  /* setup() is watched far more loosely than the running loop. A LittleFS
     format, an identity generation, or the SoftDevice role ladder are all
     legitimately slow, and resetting partway through one would boot-loop the
     node forever. This only has to be tighter than "never". */
  #define BOOT_WATCHDOG_MS 120000
  /* Between setup stages, so each stage gets the full boot budget to itself
     rather than all of them sharing one window. */
  #define WDOG_FEED() LoopWatchdog::feed()
#else
  #define WDOG_FEED() do {} while (0)
#endif
#if WITH_STATUS_LED
  #include <helpers/StatusLed.h>
  static StatusLed status_led;
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(board, display);
#endif

#ifdef ETHERNET_ENABLED
  #define ETHERNET_CLI_BANNER "MeshCore Repeater CLI"
  #include <helpers/nrf52/EthernetCLI.h>
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  /* Was a bare while(1). On a battery repeater that is a permanent, silent
     blackout: this is reached when radio_init() fails, before BLE or the CLI
     exist, so there is nothing left to notice it and no way in without
     physically pressing reset. A radio that failed to initialise has usually
     failed transiently -- a sagging rail on a weak battery being the obvious
     case -- and a retry from a clean boot is far more likely to succeed than
     staying wedged forever. The watchdog is armed before radio_init() now, so
     this spin is caught even if the reboot below is somehow not reached. */
  Serial.println("HALT: radio init failed, rebooting");
  Serial.flush();
  delay(2000);              // let the message out, and rate-limit a boot loop
  board.reboot();           // the portable virtual, not NVIC_SystemReset -- this
                            // file also builds for ESP32 and RP2040
  while (1) ;               // reboot() does not return; belt and braces
}

static char command[160];
#ifdef ETHERNET_ENABLED
static char ethernet_command[160];
#endif

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

#ifdef LOOP_WATCHDOG_MS
  /* Armed FIRST, before anything that can wedge.

     It used to be armed inside the_mesh.begin(), which left every unbounded
     wait in setup() unwatched: the I2C busy-spins behind display.begin() and
     the RTC probe (Wire_nRF52 polls EVENTS_STOPPED with no timeout and no
     error escape, so a slave holding SDA low after a mid-transaction reset
     spins the CPU forever), radio_init(), and the filesystem mount. Every one
     of those hangs the node with no BLE, no LoRa and no CLI -- indistinguishable
     from the power-loss case and equally unrecoverable in the field.

     The watchdog task runs at TASK_PRIO_NORMAL and setup() runs in the loop
     task at LOW, so it preempts any of those spins and reboots. */
  LoopWatchdog::begin(BOOT_WATCHDOG_MS);
#endif

  board.begin();
  WDOG_FEED();

#if defined(NRF52_PLATFORM) && defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  /* Free the I2C bus before anything touches it. A reset partway through a
     read leaves the slave holding SDA low, and the core's TWIM driver then
     spins forever on EVENTS_STOPPED with no timeout -- see I2CBusRecovery.h.
     Costs microseconds when the bus is already idle, which is the normal case. */
  if (!I2CBusRecovery::recover(PIN_WIRE_SDA, PIN_WIRE_SCL)) {
    Serial.println("I2C: bus stuck, recovery failed");
  }
#endif

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
  WDOG_FEED();
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }
  WDOG_FEED();

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }
  WDOG_FEED();

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;
#ifdef ETHERNET_ENABLED
  ethernet_command[0] = 0;
#endif

  sensors.begin();
  WDOG_FEED();

  the_mesh.begin(fs);
  WDOG_FEED();
#if WITH_STATUS_LED
  // Colour is the radio, brightness is the direction: green = LoRa, blue = BLE
  // bridge, dim = receive, bright = transmit. Both dim together every 5s is the
  // heartbeat. The RAK3401 has only these two LEDs and no red.
  status_led.begin(LED_BLUE, LED_GREEN, LED_STATE_ON);
#endif

#if WITH_BLE_CLI
  // Local diagnostic and firmware-update port -- SerialBLEInterface also brings
  // Adafruit's DFU service with it. On a repeater sited with no USB attached
  // this is the ONLY way back in, including for the next OTA, so a build that
  // can reach such a node must not drop it.
  {
    static SerialBLEInterface ble;
    static char ble_name[32];
    strncpy(ble_name, "@@MAC", sizeof(ble_name) - 1);   // resolved to the MAC by begin()
    the_mesh.startBLE(ble, "MeshCore-", ble_name);
  }
#endif

#if defined(WITH_BLE_BRIDGE)
  // The bridge cannot bring the stack up from its own begin(), which runs
  // inside the_mesh.begin() above -- anything that starts BLE afterwards would
  // clobber the raw event callback it depends on. So the host does it here,
  // once whoever else wants BLE has had their turn.
  #if WITH_BLE_CLI
    // The CLI already started the stack and claimed the raw-event callback,
    // which is a single slot. Hand it to the bridge so it can forward every
    // event it does not consume rather than silently swallowing the CLI's.
    BLEBridge::setBleReady(SerialBLEInterface::onBLEEvent);
  #else
    // Nothing else uses BLE on this build, so the bridge owns the stack and
    // there is no chain to forward to.
    {
      char ble_name[40];
      snprintf(ble_name, sizeof(ble_name), "MeshCore-%s", the_mesh.getNodePrefs()->node_name);
      BleBroadcast::initStack(ble_name);
      BLEBridge::setBleReady(NULL);
    }
  #endif
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef ETHERNET_ENABLED
  ethernet_start_task();
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  // Handle Serial CLI
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    reply[0] = 0;
#ifdef ETHERNET_ENABLED
    if (!ethernet_handle_command(command, reply)) {
      the_mesh.handleCommand(0, command, reply);
    }
#else
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
#endif
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#ifdef ETHERNET_ENABLED
  ethernet_loop_maintain();
  if (ethernet_read_line(ethernet_command, sizeof(ethernet_command))) {
    char reply[160];
    reply[0] = 0;
    if (!ethernet_handle_command(ethernet_command, reply)) {
      the_mesh.handleCommand(0, ethernet_command, reply);
    }
    ethernet_send_reply(reply);
    ethernet_command[0] = 0;
  }
#endif

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_) && !defined(DISPLAY_CLASS)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
#if WITH_STATUS_LED
  status_led.loop();
#endif
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
