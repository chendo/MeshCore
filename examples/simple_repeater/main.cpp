#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"
#if WITH_STATUS_LED
  #include <helpers/StatusLed.h>
  static StatusLed status_led;
#endif
#ifdef NRF52_PLATFORM
  #include <helpers/nrf52/I2CBusRecovery.h>
#endif
#ifdef LOOP_WATCHDOG_MS
  #include <helpers/nrf52/LoopWatchdog.h>
  /* The limit for setup() is much longer than the limit for the running loop.
     A LittleFS format is slow. The node also needs time to make a new
     identity, and to climb the SoftDevice role ladder. These delays are
     correct. A reset in the middle of one of them starts a boot loop. */
  #define BOOT_WATCHDOG_MS 120000
  #define WDOG_FEED() LoopWatchdog::feed()
#else
  #define WDOG_FEED() do {} while (0)
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

#ifdef LORA_WATCHDOG_MS
#include <helpers/LoraWatchdog.h>
/* The watchdog belongs to the board. One board has one watchdog, whatever
   identities run on it. The file helpers/LoraWatchdog.h holds the stages that
   the watchdog steps through. The functions below are the three actions that
   the watchdog performs on this node. */
static LoraWatchdog lora_watchdog;

static uint32_t lora_wd_airtime(void*) {
  return (uint32_t)(the_mesh.getTotalAirTime() + the_mesh.getReceiveAirTime());
}
static void lora_wd_probe(void*) {
  the_mesh.sendSelfAdvertisement(500, false);   // A zero-hop advert. It is cheap and it does not flood.
}
static void lora_wd_reinit(void*) {
  /* This function applies every parameter that begin() applies. A call to
     radio_init() alone leaves the driver on its default frequency. The node
     then transmits off-band and gives no warning. That result is worse than
     the fault that this function repairs. */
  NodePrefs* p = the_mesh.getNodePrefs();
  radio_init();
  radio_driver.setParams(p->freq, p->bw, p->sf, p->cr);
  radio_driver.setTxPower(p->tx_power_dbm);
  radio_driver.setRxBoostedGainMode(p->rx_boosted_gain);
  board.setLoRaFemLnaEnabled(p->radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(p->radio_fem_txgain);
}
static void lora_wd_reboot(void*) {
  the_mesh.flushPendingWrites();
  board.reboot();
}
#endif

void halt() {
  /* This function held a bare while(1) before. The code reaches it when
     radio_init() fails. At that point BLE and the CLI do not yet exist. An
     installed repeater therefore hangs and stays silent. Nobody can reach it.
     Only a person at the site can press the reset button. A radio init failure
     is usually temporary. A weak battery that drops the supply voltage is one
     cause. A clean reboot recovers the node much more often than a hang. */
  Serial.println("HALT: radio init failed, rebooting");
  Serial.flush();
  delay(2000);              // Give the message time to go out. Also slow a boot loop.
  board.reboot();           // A portable virtual call, not NVIC_SystemReset. This
                            // file also builds for ESP32 and RP2040.
  while (1) ;
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
  /* The watchdog starts before any step that can hang. Before this change, the
     code started the watchdog inside the_mesh.begin(). Several steps were then
     unwatched: the I2C busy-spins behind display.begin() and behind the RTC
     probe, radio_init(), and the filesystem mount. Each of these steps hangs
     the node. The node then has no BLE, no LoRa and no CLI. The watchdog task
     runs at TASK_PRIO_NORMAL. setup() runs in the loop task at LOW priority.
     The watchdog task therefore preempts any of these steps. */
  LoopWatchdog::begin(BOOT_WATCHDOG_MS);
#endif

  board.begin();
  WDOG_FEED();

#if defined(NRF52_PLATFORM) && defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  /* A reset in the middle of a read leaves the slave with SDA held low. The
     TWIM driver in the core then spins for ever on EVENTS_STOPPED. That driver
     has no timeout. See I2CBusRecovery.h. This check costs only microseconds
     when the bus is already idle. */
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
#ifdef LORA_WATCHDOG_MS
  lora_watchdog.begin(LORA_WATCHDOG_MS, NULL, lora_wd_airtime, lora_wd_probe,
                      lora_wd_reinit, lora_wd_reboot);
#endif
#if WITH_STATUS_LED
  // The colour shows the radio. The brightness shows the direction. Green is
  // LoRa. Blue is the BLE bridge. Dim is receive. Bright is transmit. Both LEDs
  // go dim together every 5s for the heartbeat. The RAK3401 has only these two
  // LEDs. It has no red LED.
  status_led.begin(LED_BLUE, LED_GREEN, LED_STATE_ON);
#endif
  WDOG_FEED();

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

#ifdef LOOP_WATCHDOG_MS
  LoopWatchdog::feed();
  /* The short limit for the running loop is safe only after the loop runs one
     time. Until this point the boot limit covers setup(). */
  static bool wdog_tightened = false;
  if (!wdog_tightened) {
    wdog_tightened = true;
    LoopWatchdog::setLimit(LOOP_WATCHDOG_MS);
  }
#endif

  the_mesh.loop();
#ifdef LORA_WATCHDOG_MS
  lora_watchdog.loop();   // The watchdog sets its own rate. See LoraWatchdog::CHECK_EVERY_MS.
#endif
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
