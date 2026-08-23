# MeshCore-Hydra

This page describes everything that MeshCore-Hydra adds to MeshCore. The
[README](../README.md) gives the short version and the CLI reference.

The base is upstream `v1.17.1` (`d9296435`).

---

## Status, and what you can trust

Read this part first.

**No firmware from this tree has run on real hardware. Not one time.** Every claim on
this page is a build result or a host test against a mock. "The build passes" and "we
tested it" are two different claims, and this page keeps them apart. `SharedRadio` has
90 host test cases that drive a mock `mesh::Radio`. The arbiter has never driven a real
SX1262. Do not make a remote repeater your first target.

**Three boards carry these features: the RAK3401, the Elecrow ThinkNode M1 and the
Elecrow ThinkNode M5.** The M1 is a second nRF52840 with the same SoftDevice, so it gets
everything the RAK3401 gets except the status LED. The M5 is an ESP32-S3, so it gets the
portable half. `LoopWatchdog` and `I2CBusRecovery` are nRF52 code, and the Bluetooth
bridge needs the SoftDevice; none of the three run on the M5. `StatusLed` needs two LEDs
that it can drive with hardware PWM, and only the RAK3401 has them.

| feature | RAK3401 | ThinkNode M1 | ThinkNode M5 |
|---|---|---|---|
| hydra (`SharedRadio`, slots, `MeshObserver`) | yes | yes | yes |
| `LoraWatchdog` | yes | yes | yes |
| `LoopWatchdog`, `I2CBusRecovery` | yes | yes | no, nRF52 only |
| Bluetooth bridge | yes | yes | no, needs the SoftDevice |
| `StatusLed` | yes | no, one usable LED | no, the LEDs sit on an I2C expander |

**The room server is not complete.** A room slot gets its own identity, its own ACL and
its own post buffer, and it adverts as a room. The room protocol itself is not written.
That protocol delivers the posts, syncs the clients and retries the pushes. A chat slot
boots, adverts and collects contacts. It answers no messages yet.

**Some data has no command that reports it.** The `peers` command draws the peer table.
The radio driver holds a receive-failure count for each cause. The packet trace records
each receive error with its RadioLib code. No command prints either of those two. That
work is still open.

---

## Two upstream bug fixes

Both are plain upstream bugs. Both sit on their own branch on top of `v1.17.1`, one
change each, and are ready to send as pull requests.

**A packet pool with no lock** (`fix/packet-pool-locking`). Two FreeRTOS priorities
drive `StaticPoolPacketManager` on nRF52: the Arduino loop at `TASK_PRIO_LOW`, and the
Bluefruit BLE task at `TASK_PRIO_HIGH`. Nothing synchronised them. `add()` tested
`_num == _size`. That test is a latch. One lost update pushes `_num` past `_size`, the
test never matches again, and every later `add()` writes past the end of three heap
arrays for the rest of the boot. The fix adds a nested `PacketQueueLock` around
`countBefore`, `get`, `itemAt`, `removeByIdx` and `add`, changes the test to `>=`, and
checks the bounds in `removeByIdx`. The lock uses `taskENTER_CRITICAL` on nRF52 and does
nothing elsewhere. The bug affects anyone who runs BLE and the mesh together.

**A flash write on each login** (`fix/acl-flash-writes`). Each login with a password
reached the contacts write path. A repeat login by an admin already in the ACL reached
it too. A flash write holds the main loop for about 1.6 s while the SoftDevice
arbitrates. That, and not mesh traffic, caused a worst-case loop gap of about 2 s on two
repeaters in the field. The fix reads `was_known` and `prev_perms` before the code
overwrites the fields, and marks the ACL dirty only for a new client or for a changed
permission. The cost is stated: the code no longer pushes `last_timestamp` to flash on
each login, so its replay protection can go back to the last stored value across a
reboot. That guarantee was already weak, because the write was lazy. A permission change
still goes to flash immediately.

---

## The link metrics, in upstream files

`src/Dispatcher.h` and `src/Packet.h` are **not** untouched, and neither are
`src/Dispatcher.cpp` and `src/Packet.cpp`. Each gained a small addition, on purpose, and
all four are headed for one upstream pull request.

`Packet.h` gains two link-metric fields, `_rssi` (16 bit) and `_cr`, beside the `_snr`
that upstream already had. The widest field comes first, so all three fit in the two
bytes of tail padding that the struct already had. Another order costs 4 bytes. The two
new fields are local receive data. `writeTo()` does not encode them and `readFrom()`
does not read them. They stay at zero for a packet that this node builds itself. The
accessors are `getRSSI()` and `getCodingRate()`.

`Packet.cpp` sets the three fields to zero in the constructor, which is what makes the
sentence above true.

`Dispatcher.h` holds the `mesh::Radio` interface, and that interface gains two virtuals.
Each one has a default body, so every `mesh::Radio` subclass that exists still compiles.
`getLastRxCodingRate()` returns the coding rate of the last frame that the radio decoded.
It returns 0 by default, which means "unknown". `getEstAirtimeForCR()` prices the airtime
at the coding rate of the sender. It returns our own figure by default.
`CustomSX1262Wrapper.h` implements both of them for the SX1262.

Why the coding rate matters: in explicit-header mode each frame carries its own rate,
which is the choice of the sender and does not have to match ours. A frame at 4/8 holds
the channel 60% longer than the same bytes at 4/5.

`Dispatcher.cpp` reads the three values in `checkRecv()`, in one place, before anything
else touches the radio. The modem latches them only until it decodes the next frame. It
also prices `rx_air_time` at the coding rate of the sender, because that is the time that
the frame held the channel.

`src/Mesh.h` and `src/MeshCore.h` are identical to `v1.17.1`.

---

## Work that keeps a node alive

### The node protects its own boot

A repeater on a site can become a brick that nobody can recover. There are three ways.
All three share one cause: nothing watched the node before the main loop ran.

- **`LoopWatchdog` arms first.** `setup()` arms it before `the_mesh.begin()`. Everything
  before that point could wait forever: `radio_init()`, the filesystem mount, and the
  I2C spins behind `display.begin()` and the RTC probe. A stop in any of them leaves no
  BLE, no LoRa and no CLI. `setup()` gets a loose limit of 120 s. The tight run-time
  limit applies only after the loop runs one time.
- **`I2CBusRecovery` frees a wedged bus.** A reset in the middle of a read can leave a
  slave device that holds SDA low. The TWIM driver of the Adafruit core polls
  `EVENTS_STOPPED` with no timeout and no escape (`Wire_nRF52.cpp:175`, `:241`). That
  slave then stops the CPU on the first transfer of **each later boot**. The recovery
  code clocks the slave out.
- **`halt()` reboots.** It held a bare `while(1)` before. The code reaches it when
  `radio_init()` fails, which is usually temporary. A weak battery is one cause. A clean
  reboot recovers the node much more often than a stop does.

This work deliberately does **not** use the nRF52 hardware watchdog. You cannot stop
that watchdog once it starts, and it survives a soft reset. It would keep the count
through a BLE firmware update of about 200 s, on hardware whose only remote route in is
that update.

The feed and the change from the boot limit to the run-time limit live in `main.cpp`,
and not in `MyMesh`. A node with several mesh instances thus gets one watchdog, and not
one for each instance.

### The LoRa radio watchdog

From the outside, a quiet band and a dead radio look the same. Repeaters have been found
with a transceiver that sent nothing and received nothing for more than an hour. The
configuration was correct and a reboot did not repair it. You can tell the two apart
only if the node makes its own traffic. `LoraWatchdog` does that, in stages:

```
15 min with no airtime  ->  send one zero-hop advert
30 s grace              ->  did the airtime move?  the radio is alive
                        ->  no: run radio_init() again, then restore the
                                frequency, bandwidth, spreading factor, coding
                                rate, TX power, RX gain and FEM gains
30 s grace              ->  did the airtime move?  the radio is alive
                        ->  no: write the ACL to flash, then reboot
```

The restore matters. A bare `radio_init()` leaves the driver on its default frequency,
and a node that is silently off band is worse than the fault that it repaired. The
watchdog checks one time every 30 s. Every elapsed-time comparison is signed. An unpaced
retry and an unsigned elapsed-time test have each already cost a node.

The watchdog is **node scoped**. A per-identity watchdog reasons from the airtime of its
own `Dispatcher`, which under a shared radio is one identity's share of the traffic. It
can also reboot the whole board. So the module takes its three actions — probe, reinit
and reboot — as injected functions, and the node supplies one of each. The
`LORA_WATCHDOG_MS` flag both turns the feature on and sets the idle time before a probe.
It is set to 15 minutes on `RAK_3401_repeater_hardened`, `RAK_3401_repeater_bridge_ble`,
`RAK_3401_hydra`, `ThinkNode_M1_repeater_hardened`, `ThinkNode_M1_repeater_bridge_ble`,
`ThinkNode_M1_hydra`, `ThinkNode_M5_Repeater_hardened` and `ThinkNode_M5_hydra`. The
module holds no nRF52 code, so the M5 runs the same watchdog that the nRF52 boards run.
Every one of those envs is defined in `variants/hydra_rak3401`, `variants/hydra_m1` or
`variants/hydra_m5`; the `_hardened` envs extend the stock repeater env of the board and
add the watchdogs to it, so upstream's own envs stay untouched.

### The status LED

The RAK3401 has exactly two LEDs, green and blue. It has no red LED. Rather than mix a
colour that the board cannot make, the code uses the two axes that it does have. **The
colour is the radio. The brightness is the direction.**

|       | dim (receive) | bright (transmit) |
|-------|---------------|-------------------|
| green | LoRa RX       | LoRa TX           |
| blue  | bridge RX     | bridge TX         |

Both LEDs go dim together one time every 5 s as a heartbeat. The brightness is true
hardware PWM (`analogWrite`), so the levels do not change with how often `loop()` runs.
The `WITH_STATUS_LED` flag turns it on.

**Neither ThinkNode board turns it on, and each has its own reason.** The M1 has exactly
two LEDs, and **no green one**: blue on P0.13 and red on P1.04. `StatusLed` wants a blue
and a green pair, so it does not fit. Both M1 LEDs are also already spoken for. Blue is
the LoRa transmit indicator that `ThinkNodeM1Board` drives, and Bluefruit blinks the same
pin while it advertises. Red is the product-status LED, and the charger drives it in
hardware -- steady when on, fast flash while charging, slow flash on low battery.

This is worth revisiting. Until the pin-37 and LED patches landed, the values in
`variants/thinknode_m1/variant.h` were a verbatim copy of `variants/lilygo_techo/`: they
named a green LED that does not exist, put `LED_RED` on `PIN_SPI1_MISO`, and set
`LED_STATE_ON` to the T-Echo's active-low polarity, so every LED on this board ran
inverted.

The M5 puts its LEDs behind a PCA9557 I2C expander, and `ThinknodeM5Board` reaches them
with `expander.digitalWrite()`. `StatusLed` calls `pinMode()` and `analogWrite()` on a
GPIO, and no hardware PWM reaches an I2C expander. The blue LED on expander pin 1 is
also already the LoRa transmit indicator of that board.

To light either board a redesign is needed, not a build flag: `StatusLed` would have to
accept a pin writer rather than a pin number, and it would have to fall back to on/off
where no PWM exists.

---

## SharedRadio

`src/helpers/SharedRadio.{h,cpp}`, 1,175 lines, 90 host test cases plus 8 more on a
second env.

Several `mesh::Mesh` instances — separate identities, each with its own keys, prefs and
dispatcher — on one board with one physical LoRa radio. `SharedRadioCore` owns the real
driver. Each mesh gets a `RadioPort`, which **is** a `mesh::Radio`, and the core
arbitrates between the ports. The limit is 8 ports.

- **RX fan-out.** The modem holds exactly one frame, so the core empties the radio the
  moment a frame lands. It puts the frame in a short queue and gives it one time to each
  port that listens. Each identity sees all of the traffic. Each identity then decides on
  its own whether the frame is for it.
- **One transmit at a time.** The core refuses `startSendRaw()` from a sibling, and
  `isReceiving()` reads busy for that sibling. The stock CAD and back-off path therefore
  defers the sibling with no change to `Dispatcher`. A transmit
  that never completes is force-released, because `pump()` does not touch the radio
  while a send is in flight. A wedged send would otherwise take the whole board off air.
- **A pooled duty-cycle budget.** `tx_budget_ms` lives in `Dispatcher`, and **each
  `Mesh` instance owns one**. N identities that each believe that they hold the whole
  duty cycle let the node transmit at N times its share. The pool now lives in
  `SharedRadioCore`, which is the only thing that sees every transmit. The
  `airtime_factor` of slot 0 sets the size of that pool. One test fails if anything ever
  over-draws the pooled budget.
- **TX wait counters.** Eight counters cover each path that defers a send: the pooled
  budget is empty, a sibling held the transmitter, the port yielded to a higher
  priority, the driver refused, the port forced the send through, and the three
  different conditions that all read as a busy channel — a preamble or header interrupt,
  an RSSI above the noise floor, and a hardware CAD result. A node that cannot get a word
  in used to look the same as a node on a quiet band. Read the counters with
  `stats-txwait`.
- **Loopback.** One antenna is half duplex, so two identities on the same board cannot
  hear each other over the air. A chat identity could therefore never address the
  repeater or the room on its own board. With loopback on, the core also gives each
  transmitted frame to the sibling ports as a received frame, and never back to the
  sender. Each identity then has the experience of a second physical node in the same
  room. Hydra turns loopback on.
- **Loopback frames are delivered but never relayed.** A node that can hear the room slot
  of this board can also hear the repeater of this board: one antenna, one radio horizon.
  A relay of a sibling therefore adds no coverage. It only spends airtime from the pooled
  duty budget and puts an extra hash in the path, which makes one board look like two
  hops. So the arbiter marks each loopback frame, `RadioPort::lastRxWasLoopback()`
  reports the mark to the identity that takes the frame, and `LoopbackForwardGuard`
  carries it as far as `allowPacketForward()`, which is where a mesh decides to
  retransmit. A frame off the air is forwarded exactly as before. `stats-shared` reports
  `lbmiss`, which counts any mark that the guard could not hold; it should stay at zero.
- **Cross-slot priority**, set for each port and not fixed in the arbiter, so the policy
  belongs to the caller. Hydra gives slot 0 priority 0 and gives the rest priority 1. The
  mesh depends on the repeater. It does not depend on chat.
- **Reconciled collision settings.** Three dispatchers that each push their own
  `cad_enabled` and `interference_threshold` at one radio would let an identity with CAD
  off turn CAD off for all of the others. The core reconciles the settings instead.

The arbiter sits **below** `mesh::Radio`. It does not teach `Mesh` about several
identities. `mesh::Radio` is already the seam between a `Dispatcher` and its
transceiver, and the transceiver is the only thing that the identities contend for. It
is enough to implement that one interface.

---

## The hydra node role

`examples/hydra/`, 1,898 lines across 10 files. One board, one LoRa radio, N mesh
identities.

```
HydraNode
├── board, radio driver, RTC, RNG, filesystem
├── SharedRadioCore ── one RadioPort for each slot
├── node modules: LoopWatchdog, LoraWatchdog, StatusLed, I2CBusRecovery, MeshObserver
└── slots[HYDRA_NUM_SLOTS]
      slot 0    : repeater — always on
      slot 1..N : chat | room | off
```

Slot 0 runs the **same `simple_repeater` `MyMesh` that this tree ships**. It is not a
fork of it. `MyMesh` already takes a `mesh::Radio&`, so the reuse is to hand it a
`RadioPort` in place of `radio_driver`. The build compiles that directory's `MyMesh.cpp`
and puts the directory on the include path. To upgrade a `simple_repeater` node
therefore costs nothing: slot 0 keeps the `_main` identity key, so `prefs.json`,
the ACL and the region map carry over. Flash hydra over a repeater and it comes back as
the same node with slots 1 and above off.

Slots 1 and above are much smaller than slot 0: a packet pool of 8 against 32, and a
dedup table of 32 hashes against the 160 of `SimpleMeshTables`. `MAX_CONTACTS` is 16,
against the 350 of the stock RAK3401 companion env. A chat identity is an end point and
forwards nothing, because slot 0 already forwards everything that this radio hears. Two
identities that both forwarded the same flood would put two copies of it on one antenna.

Slot objects are static, and every port registers with the arbiter at boot whether the
slot is on or off. The port index thus equals the slot index for the life of the boot.
The packet pool is the only part on the heap, and `begin()` allocates it. A slot that is
off costs no heap. A slot that you enable after months of uptime asks the heap for one
block, rather than lose to fragmentation piece by piece.

`HYDRA_RAM_RESERVE` (24,576 B) is a node floor. `RamFloor` pins the reserve before a
slot allocates, so a slot that starts has kept the floor intact. The node refuses a slot
that cannot keep it. It refuses that slot now, and does not drop packets at 3 a.m.

The slot config lives at `/hydra_slots`. Each failure path lands on "a plain repeater
with the chat slots off": an absent file returns early, a bad `'H',2` magic keeps the
defaults, a short read keeps what it got, and an unknown slot type becomes `SLOT_OFF`
rather than a different type.

Build flags in `variants/rak3401/platformio.ini`, `variants/thinknode_m1/platformio.ini`
and `variants/thinknode_m5/platformio.ini` configure hydra: `HYDRA_NUM_SLOTS`
(1 to 8), `HYDRA_CHAT_POOL`, `HYDRA_CHAT_HASHES`, `HYDRA_RAM_RESERVE`, `MAX_CONTACTS` and
`MAX_CONNECTIONS`. `HYDRA_NAME_PREFIX` has one use only: it names a slot that a version 1
config file carried with no name. It does not name a new slot. A new slot has no name
until the operator sets one.

---

## MeshObserver

`src/helpers/MeshObserver.{h,cpp}`, 953 lines. It reports link topology, not traffic
volume. It knows nothing about identities, radios or arbitration. The caller feeds it
raw frames and it reports what it saw. It therefore works on a hydra node, where
`SharedRadioCore` feeds it, and it would work on a plain single-identity repeater, where
a receive hook would feed it. Today only the hydra build feeds it.

Each forwarder adds its own hash to the end of the path of a packet
(`Mesh::routeRecvPacket`). That makes two facts recoverable. RF links are asymmetric, so
the two facts are genuinely different:

- the **last** entry in a path transmitted the frame that we received, so **we can hear
  it**, and the SNR and RSSI of this frame describe that link;
- the entry **directly after one of ours** received our transmission and relayed it, so
  **it can hear us**.

An entry in the middle of a path says nothing about its own link to us, and the code
never reads it that way. The originator of a packet chooses the hash width, so the same
node appears at one byte and at two. A match at one byte is a coincidence one time in
256. The code counts it separately, never calls it proof, and attributes it to a known
wider peer only when exactly one candidate exists. The peer table holds 48 entries.

Identity comes from adverts: a pubkey prefix, a name and a location. A path carries only
truncated hashes, so a node stays anonymous until one of its adverts reaches us. An
advert heard at zero hops also measures that node's clock directly against ours.

`examples/hydra/PeerReport.h` draws the table for the `peers` command. It belongs to the
hydra example, and not to the observer. It is a separate, host-testable file, because a
fault in the format would destroy the two facts that the table exists to show, and
nothing would report the fault.

---

## The Bluetooth bridge

`src/helpers/nrf52/Ble{Stack,Discovery,Link}.{h,cpp}` and
`src/helpers/bridges/BLEBridge.{h,cpp}` with `BleBridgeFrame.h`, 1,647 plus 902 lines.

It joins two nRF52840 repeaters over a Bluetooth connection, so a packet that one node
hears on its LoRa band goes out again on the other band. It is the nRF52 counterpart to
the ESP-NOW bridge, for boards that have no WiFi.

The transport is a connection, not a broadcast. An earlier experiment carried datagrams
over BLE 5 extended adverts and could not be made reliable: one advert lands about half
of the time, and three to five copies move the loss from 49% to 3.4% and never to zero,
because nothing acknowledges anything. A connection acknowledges each packet and sends
it again. A frame on the link is up to 256 bytes, so a full MeshCore packet fits in one.

For the wire format, the group marker, the deny list, the settings and the threat model,
see **[ble_bridge.md](ble_bridge.md)**. This page does not repeat them.

`RAK_3401_repeater_bridge_ble` and `ThinkNode_M1_repeater_bridge_ble` are the envs that
enable the bridge, defined in `variants/hydra_rak3401` and `variants/hydra_m1`. Both boards carry an nRF52840 with s140 6.1.1 and link against
`boards/nrf52840_s140_v6.ld`, so the SoftDevice gets the same 24 KB at RAM origin
`0x20006000` on each, and each env asks for the same 2 peripheral and 2 central slots.
`BleStack::ensure()` still finds the true ceiling at boot and steps down to what fits, so
those numbers stay a request on both boards and never an assumption.

---

## Build results

The host is aarch64 Linux with the toolchain override that the README describes, so the
nRF52 sizes differ by a few percent from the official release artifacts.

The flash ceiling is the whole application region on the nRF52 boards, and one OTA slot
on the M5.

| env | result | flash | RAM |
|---|---|---:|---:|
| `RAK_3401_repeater_hardened` | builds | 380,092 B (46.6% of 815,104) | 33,048 B (14.0% of 235,520) |
| `RAK_3401_repeater_bridge_ble` | builds | 395,848 B (48.6%) | 41,200 B (17.5%) |
| `RAK_3401_hydra` (3 slots) | builds | 392,876 B (48.2%) | 54,600 B (23.2%) |
| `RAK_3401_hydra_debug` (packet trace on) | builds | 395,500 B (48.5%) | 65,544 B (27.8%) |
| `ThinkNode_M1_repeater_hardened` | builds | 304,848 B (37.4% of 815,104) | 30,768 B (13.1% of 235,520) |
| `ThinkNode_M1_repeater_bridge_ble` | builds | 320,412 B (39.3%) | 38,904 B (16.5%) |
| `ThinkNode_M1_hydra` (3 slots) | builds | 330,128 B (40.5%) | 52,512 B (22.3%) |
| `ThinkNode_M5_Repeater_hardened` | builds | 1,126,385 B (85.9% of 1,310,720) | 60,912 B (11.6% of 524,288) |
| `ThinkNode_M5_hydra` (5 slots) | builds | 1,125,249 B (57.2% of 1,966,080) | 104,288 B (19.9%) |

The ESP32 image embeds the `.pio/libdeps/<env>/...` path of each third-party source that
uses `__FILE__`, so an M5 flash figure moves by a few bytes when an env is renamed.
`ThinkNode_M5_Repeater_hardened` is 24 B larger than the same build under a 21-character
name for that reason alone.

The M1 rows sit below the RAK3401 rows because those envs drive no display and pull in
no sensor drivers. The M5 rows are far larger than either, because that image carries the
ESP32 WiFi stack for the over-the-air update endpoint.

**The M5 partition table.** `ThinkNode_M5_Repeater_hardened` keeps the stock table for a
4 MB part and fills 85.9% of its 1,310,720 B OTA slot. `ThinkNode_M5_hydra` moves to
`min_spiffs.csv`, a stock table of the platform that four other variants in this tree
already use. It gives each OTA slot 1,966,080 B and leaves 131,072 B of SPIFFS, which is
far more than the identities, the preferences, the ACLs and `/hydra_slots` need.
`huge_app.csv` would give 3 MB, but it deletes the second OTA slot, and a repeater on a
mast is the node that most needs to accept an image over the air. The hydra env also
drops `DISPLAY_CLASS`, which keeps GxEPD2 and its fonts out of the image. That reduction
is about the size of hydra itself, so the two envs land within 1,200 B of each other.

**A build that passes is not a test on hardware.** No firmware from this tree has gone
onto a board.

### The cost against upstream

Upstream `v1.17.1` (`d9296435`) built from its own tree on the same host, in the same
env:

| RAK3401 repeater | upstream `RAK_3401_repeater` | this fork `RAK_3401_repeater_hardened` | delta |
|---|---:|---:|---:|
| flash | 376,780 B | 380,092 B | **+3,312 B** |
| RAM | 32,928 B | 33,048 B | **+120 B** |

### The cost of a chat slot

Measured by a build of `RAK_3401_hydra` at `HYDRA_NUM_SLOTS=2` and again at 3:

| for each chat slot | cost |
|---|---:|
| static (`.bss`), reserved whether the slot is on or off | +5,640 B |
| flash | none that the build can measure (−16 B) |
| heap, only while the slot is **on** | 2,352 B (8 × `sizeof(mesh::Packet)` = 262, plus 256) |

A room slot also allocates a `RoomStore` on the heap, which holds the ACL and the post
buffer.

Most of the 5,640 B is the contact table of `BaseChatMesh`, so `MAX_CONTACTS` is the
dial that matters if that figure has to come down.

The same measurement on `ThinkNode_M5_hydra`, at 3 slots and again at 5, gives **9,096 B
of static RAM for each chat slot** and 4 B of flash for the pair. That figure is higher
than the 5,640 B of the nRF52 envs because the M5 sets `MAX_CONTACTS=32` where they set
16, and the contact table of `BaseChatMesh` is most of the cost.

Slot counts follow the RAM of the part. The nRF52 boards hold 235,520 B and take 3 slots.
The M5 holds 524,288 B, so a chat slot costs it 1.7% where the same slot costs an nRF52
2.4%, and that env takes 5. Above five the antenna is the limit and not the RAM: slot 0
already forwards every flood that the radio hears, and each further identity adds only
its own advert schedule to one half-duplex antenna.

### The cost of the packet trace

The diagnostic packet trace costs **10,952 B of RAM and 2,624 B of flash** at 48 entries
of 200 raw bytes. It is off unless a build asks for it. `RAK_3401_hydra_debug` asks for
it. Compare that with the whole cost of hydra: `RAK_3401_hydra` at 3 slots uses 21,496 B
of RAM more than `RAK_3401_repeater_hardened`, and the two chat slots hold 11,280 B of
that.

---

## Tests

```bash
pio test -e native -e native_ble -e native_radio -e native_multi \
         -e native_multi_notrace -e native_kiss_modem
```

**277 cases, and all of them pass.** 229 are new here. 48 are upstream's.

| suite | env | cases |
|---|---|---:|
| `test_shared_radio` | `native_multi` | 90 |
| `test_hydra_slots` | `native_multi` | 53 |
| `test_lora_watchdog` | `native_multi` | 18 |
| `test_ble_bridge` | `native_ble` | 18 |
| `test_radio_wrapper` | `native_radio` | 17 |
| `test_mux` | `native_multi` | 14 |
| `test_packet` | `native` | 8 |
| `test_shared_radio_notrace` | `native_multi_notrace` | 8 |
| `test_bridge` | `native` | 3 |

`test_shared_radio_notrace` needs its own env, because a second binary is the only way
to exercise the off state of a compile-time flag. `native_ble` supplies a real SHA-256
in place of the deliberately fake one that the other host tests share, because the
bridge frame carries an HMAC tag. The Bluetooth code itself needs the SoftDevice and
cannot run on a host.

Every test runs against a mock `mesh::Radio` and mock `Arduino` headers. This is the
only real safety net that the fork has, and it is worth a plain statement of what it
covers: the logic of the arbiter, and not its behaviour on hardware.

---

## The relationship with upstream

This fork is meant to be **upstream-eligible**. That is a constraint that shapes the
code. It is not a policy statement.

- **New behaviour arrives as new files plus a build flag**, and never as a changed
  default. The flags include `LOOP_WATCHDOG_MS`, `LORA_WATCHDOG_MS`, `WITH_STATUS_LED`,
  `BRIDGE_CLASS`, `PKT_TRACE_ENTRIES` and the `HYDRA_*` family.
- **Anything that truly needs a core change becomes its own small upstream pull request
  first**, rather than a fork delta that we carry. The additions to `Dispatcher.h` and
  `Packet.h` are on that route now.
- **We rebase onto upstream from time to time**, and not only at pull-request time. The
  point of the constraints above is that the rebase stays close to free.

The full diff against `v1.17.1` is 111 files, +12,862 and −390 lines. New files hold
11,577 of the added lines: 31 source files hold 7,322, tests and mocks hold 4,022, and
`docs/ble_bridge.md` holds 233. That leaves 1,285 lines added to files that already
existed. Much of that 1,285 is a rewrite of the comments across the tree into Simplified
Technical English, and not new behaviour.

This fork changes 20 upstream source files. They fall into four groups.

**The link metrics.** `src/Dispatcher.{h,cpp}`, `src/Packet.{h,cpp}`,
`src/helpers/radiolib/RadioLibWrappers.{h,cpp}` and
`src/helpers/radiolib/CustomSX1262Wrapper.h`. The wrapper files also count the receive
failures by cause. `src/Mesh.h` and `src/MeshCore.h` are unchanged.

**The two bug fixes.** `src/helpers/StaticPoolPacketManager.{h,cpp}` holds the pool lock.
The ACL flash-write fix is in `examples/simple_repeater/MyMesh.cpp`.

**The choice of a bridge.** `src/helpers/AbstractBridge.h`, `src/helpers/CommonCLI.{h,cpp}`,
`src/helpers/bridges/BridgeBase.h`, `src/helpers/bridges/RS232Bridge.{h,cpp}` and
`src/helpers/bridges/ESPNowBridge.{h,cpp}`. The `platformio.ini` of each bridged variant
changes with them. A build now names its bridge with `BRIDGE_CLASS`, in the way that it
already named its radio and its display. This is more than a macro rename: the
`CommonCLICallbacks` interface lost `setBridgeState()` and `restartBridge()` and gained
one `getBridge()`.

**The repeater example.** `examples/simple_repeater/` — `MyMesh.{h,cpp}` and `main.cpp`,
178 added lines and 42 removed. Beside the ACL fix, these wire up the watchdogs and the
LED, and move the state machine of the LoRa watchdog out into its own module. Hydra
itself does not need those changes. Slot 0 reuses `MyMesh` as it stands.

---

## One new module that no firmware uses yet

`src/helpers/esp32/MuxSerialInterface.h`, 173 lines, 14 host test cases. It lets a second
local client — a web UI or a bot — drive the app protocol of a companion identity over
the same serial interface that the phone app uses. It is ESP32 code, it is header only,
and **no build env compiles it**. Only `test_mux` exercises it. It is counted in the
figures above.
