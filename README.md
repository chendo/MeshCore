# MeshCore-Hydra

A fork of [MeshCore](https://github.com/meshcore-dev/MeshCore) that adds node-level
reliability work and a multi-identity node role, kept deliberately close to upstream.

Base: upstream `v1.17.1` (`d9296435`). Everything here is either a bug fix offered back
upstream, a new file, or a new build flag. The four core headers — `src/Mesh.h`,
`src/Dispatcher.h`, `src/Packet.h`, `src/MeshCore.h` — are byte-for-byte upstream.

Two things this fork is about:

1. **A repeater should not need a site visit.** Three unrecoverable-brick paths and one
   dead-radio failure mode are closed, plus two upstream bugs found while chasing them.
2. **One board, one radio, several mesh identities.** `SharedRadio` arbitrates a single
   transceiver between several `mesh::Mesh` instances; the `hydra` role builds a node on
   top of it — slot 0 a repeater, slots 1..N chat identities.

## Status

Early. Read this before deciding it is worth your time.

- **Nothing has been run on real hardware.** Not once. Every claim below is
  compile-verified plus host tests against mocks. `SharedRadio` has 100 host test cases
  driving a mock `mesh::Radio`; the arbiter has never driven a real SX1262.
- The reliability features are **RAK3401 (nRF52840) only** so far. `LoopWatchdog` and
  `I2CBusRecovery` are nRF52 code. `StatusLed` and the LoRa watchdog are portable but are
  wired into RAK3401 envs only.
- The `hydra` role has a RAK3401 env and no other. Its chat slots boot, advertise and
  track contacts. That is all they do yet.
- The room-server slot type is **not implemented**. `SLOT_ROOM` is reserved and maps to
  `SLOT_OFF`; the hook is a `RoomSlot` class alongside `ChatSlot`.
- The duty-cycle pooling fix landed before hydra could go on air, deliberately. It has
  never been checked against a real transmitter.

## What this adds over upstream

### Two upstream bug fixes

Both are plain upstream bugs, sit on their own branches parented directly on `v1.17.1`,
and are ready to submit as PRs.

**Packet-pool locking** (`fix/packet-pool-locking`, +192 B flash, +0 RAM).
`StaticPoolPacketManager` is driven from two FreeRTOS priorities on nRF52 — the Arduino
loop at `TASK_PRIO_LOW` and Bluefruit's BLE task at `TASK_PRIO_HIGH` — with no
synchronisation. `add()` tested `_num == _size`, which is a latch: once a lost update
pushes `_num` past `_size`, the guard never matches again and every subsequent `add()`
writes off the end of three heap arrays for the rest of the boot. The fix adds a nesting
`PacketQueueLock` (`taskENTER_CRITICAL`, nRF52 only, a no-op elsewhere) around
`countBefore`/`get`/`removeByIdx`/`add`, changes the guard to `>=`, and bounds-checks
`removeByIdx`. Affects anyone running BLE and mesh together.

**ACL flash writes on every login** (`fix/acl-flash-writes`, +16 B flash, +0 RAM). Every
password login reached the contacts write path, including a repeat login by an admin
already in the ACL. A flash write blocks the main loop for about **1.6 s** while the
SoftDevice arbitrates — that, not mesh traffic, was the cause of a ~2 s worst-case loop
gap observed on two production repeaters. The fix captures `was_known`/`prev_perms`
before the fields are overwritten and marks dirty only for a new client or a changed
permission grant. Documented tradeoff: `last_timestamp` is no longer pushed to flash per
login, so its replay protection can rewind to the last persisted value across a reboot.
That guarantee was already soft — the write was lazy. Permission grants still persist
immediately.

### Boot hardening

`feat/boot-watchdog`, +704 B flash / +8 B RAM. Three ways a sited repeater becomes an
unrecoverable brick, closed together because they share one cause: nothing was watching
before the main loop ran.

- `LoopWatchdog` is armed **first** in `setup()`, not inside `the_mesh.begin()`.
  Everything before that point had unbounded waits — `radio_init()`, the filesystem
  mount, and the I2C busy-spins behind `display.begin()` and the RTC probe. A hang in any
  of them leaves no BLE, no LoRa and no CLI. `setup()` gets a loose 120 s limit; the tight
  runtime limit is applied only once the loop has proved it runs.
- `I2CBusRecovery` clocks out a slave left holding SDA low by a reset partway through a
  read. The Adafruit core's TWIM driver polls `EVENTS_STOPPED` with no timeout and no
  error escape (`Wire_nRF52.cpp:175`, `:241`), so that slave wedges the CPU on the first
  transfer of **every subsequent boot**.
- `halt()` reboots instead of spinning forever. It is reached when `radio_init()` fails,
  which is usually transient.

Deliberately **not** the nRF52 hardware WDT: it cannot be stopped once started and
survives a soft reset, so it would keep counting through a ~200 s BLE DFU on hardware
whose only remote route in is that update.

The feed and the boot-to-runtime limit tightening live in `main.cpp`, not as `MyMesh`
members, so a node running several mesh instances gets one watchdog rather than one per
instance.

### LoRa radio watchdog

`feat/lora-watchdog`, +608 B flash / +32 B RAM. From outside, a silent band and a dead
radio are identical. The node distinguishes them by making its own traffic, staged:

```
15 min with no airtime  ->  send one zero-hop advert
30 s grace              ->  airtime moved?  alive
                        ->  no: radio_init() + restore freq/bw/sf/cr,
                                TX power, RX gain, FEM gains
30 s grace              ->  airtime moved?  alive
                        ->  no: flush the ACL, reboot
```

The reinit restores every parameter `begin()` applies, because a bare `radio_init()`
leaves the driver on its default frequency, and silently off-band is worse than the fault
being repaired. One check per 30 s, every elapsed-time comparison signed. An unpaced
retry and an unsigned elapsed-time test have each already cost a node.

Gated on `LORA_WATCHDOG_MS`, following `LOOP_WATCHDOG_MS`: the flag both enables the
feature and sets the idle time that starts a probe. Enabled at 15 minutes on
`RAK_3401_repeater` only; every other env is unaffected.

Not enabled on the hydra env, on purpose. A per-identity watchdog reasons from its own
`Dispatcher`'s airtime, which under a shared radio is one identity's share of the
traffic, and it can reboot the board. Moving it to node scope is outstanding work.

### Status LED

`feat/status-led`, +1,232 B flash / +32 B RAM. The RAK3401 has exactly two LEDs, green
and blue, and no red. Rather than mix a colour it cannot make: **colour is the radio,
brightness is the direction.**

|        | dim (receive) | bright (transmit) |
|--------|---------------|-------------------|
| green  | LoRa RX       | LoRa TX           |
| blue   | bridge RX     | bridge TX         |

Both dim together every 5 s as a heartbeat. Brightness is real hardware PWM
(`analogWrite`), so levels do not depend on how often `loop()` is called. Enabled via
`WITH_STATUS_LED` on `RAK_3401_repeater` and `RAK_3401_hydra`.

### SharedRadio

`src/helpers/SharedRadio.{h,cpp}`, 1,034 lines, 86 host test cases.

Several `mesh::Mesh` instances — separate identities, each with its own keys, prefs and
dispatcher — on one board with one physical LoRa radio. `SharedRadioCore` owns the real
driver. Each mesh is handed a `RadioPort`, which **is** a `mesh::Radio`, and the core
arbitrates. Up to 8 ports.

- **RX fan-out.** The core drains the radio the instant a frame lands (the modem holds
  exactly one) into a short queue, and delivers each frame to every listening port
  exactly once. Every identity sees all traffic and decides independently whether it is
  theirs.
- **TX serialisation.** One port at a time. A sibling's `startSendRaw()` is refused and
  its `isReceiving()` reads busy, so the stock CAD/back-off path defers it with no changes
  to `Dispatcher`. A transmit that never completes is force-released, because `pump()`
  will not touch the radio while one is in flight and a wedged send would otherwise take
  the whole board off air.
- **Pooled duty-cycle budget.** `tx_budget_ms` lives in `Dispatcher` and **every `Mesh`
  instance owns one**, so N identities each believing they hold the whole duty cycle let
  the node transmit at N times its share. The pool now lives in `SharedRadioCore`, the
  only thing that sees every transmission. One test fails if the pooled budget is ever
  over-drawn.
- **TX wait accounting.** Eight counters covering every path that defers a send — pooled
  budget exhausted, a sibling held the transmitter, yielded to a higher-priority identity,
  the driver refused, forced through, and the three distinct conditions that all read as
  "channel busy": preamble/header IRQ, RSSI over the noise floor, and hardware CAD. A node
  that cannot get a word in was previously indistinguishable from one on a quiet band.
  Read with `stats-txwait`.
- **Cross-slot priority**, per port rather than hard-coded, so the policy belongs to the
  caller. Hydra gives slot 0 priority 0 and the rest 1: routing is what the mesh depends
  on, chat is not.
- **Collision-avoidance settings are reconciled**, not last-writer-wins. Three dispatchers
  pushing their own `cad_enabled` / `interference_threshold` at one radio would otherwise
  let an identity with CAD off silently disable it for the others.

The arbiter sits **below** `mesh::Radio` rather than teaching `Mesh` about multiple
identities. `mesh::Radio` is already the seam between a `Dispatcher` and its transceiver,
and it is the only thing the identities actually contend for, so implementing that
interface is enough — `Mesh`, `Dispatcher` and `Packet` are untouched.

### The hydra role

`examples/hydra/`, 858 lines across 7 files. One board, one LoRa radio, N mesh
identities.

```
HydraNode
├── board, radio driver, RTC, RNG, filesystem
├── SharedRadioCore ── one RadioPort per slot
├── node-scoped modules: LoopWatchdog, StatusLed, I2CBusRecovery, MeshObserver
└── slots[HYDRA_NUM_SLOTS]
      slot 0    : repeater  — always on
      slot 1..N : chat | off      (room server reserved, not implemented)
```

Slot 0 runs the **same `simple_repeater` `MyMesh` this tree ships**, not a fork of it. It
already takes a `mesh::Radio&`, so reuse is handing it a `RadioPort` instead of `radio_driver`; the build
compiles that directory's `MyMesh.cpp` and puts it on the include path. That also means
upgrading an existing `simple_repeater` node is free: slot 0 keeps the `_main` identity
key, so `prefs.json`, the ACL and the region map carry over. Flash hydra over a repeater
and it comes back as the same node with slots 1..N off.

Slots 1..N are sized down hard against slot 0 — an 8-entry packet pool against 32, a
32-hash dedup table against 160, `MAX_CONTACTS` 16 against 350. Measured on
`RAK_3401_hydra` by building at `HYDRA_NUM_SLOTS=2` and again at 3:

| per chat slot | cost |
|---|---:|
| static (`.bss`), reserved whether enabled or not | +5,568 B |
| heap, only while **enabled** | +2,296 B |
| flash | +192 B |

About 7.9 KB all-in against slot 0's ~20 KB. Most of the 5,568 B is `BaseChatMesh`'s
contact table, so `MAX_CONTACTS` is the dial that matters if it needs to come down.

Slot objects are static and every port is registered with the arbiter at boot, active or
not, so port index equals slot index for the life of the boot. The packet pool is the
only heap part and is deferred to `begin()`, so a disabled slot costs no heap, and a slot
enabled after months of uptime asks the heap for one contiguous block rather than losing
to fragmentation piecemeal.

Slot config lives at `/hydra_slots` and every failure path lands on "plain repeater, chat
slots off": a missing file returns early, a bad `'H',1` magic keeps defaults, a short read
keeps what it got, and an unimplemented slot type maps to `SLOT_OFF` rather than being
silently reinterpreted.

Node CLI adds `slots`, `slot N on|off`, `slot N <cmd>`, `stats-shared` and
`stats-txwait`. Anything else falls through to slot 0, so the familiar repeater CLI still
works unqualified.

### MeshObserver

`src/helpers/MeshObserver.{h,cpp}`, 900 lines. Passive link topology, not volume stats.
It knows nothing about identities, radios or arbitration — it is fed raw frames and
reports what it saw, so it works on a hydra node (where `SharedRadioCore` feeds it) and
on a plain single-identity repeater (where a receive hook would).

Every forwarder appends its own hash to the end of a packet's path
(`Mesh::routeRecvPacket`), which makes two independent and genuinely different facts
recoverable, because RF links are asymmetric:

- the **last** entry in a path transmitted the frame we received, so **we can hear it**,
  and this frame's SNR describes that link;
- the entry **right after one of ours** received our transmission and relayed it, so **it
  can hear us**.

A mid-path entry says nothing about its own link to us and is never read that way. Hash
width is chosen by the packet's originator, so the same node turns up at one and two
bytes: a 1-byte match is a 1-in-256 coincidence, is counted separately, and is attributed
to a known wider peer only when exactly one candidate exists. 48-entry peer table.
Identity — pubkey prefix, name, location — is harvested from ADVERTs, and an advert heard
at zero hops gives a direct reading of that node's clock against ours.

It is compiled into the hydra image and fed, but barely surfaced yet: `stats-shared`
reports a peer count and nothing walks the table.

## Build status

Verified on this tree at commit `1d0f0859`. Host is aarch64 Linux with the
toolchain override described below, so nRF52 sizes differ a few percent from official
release artifacts.

| env | result | flash | RAM |
|---|---|---:|---:|
| `RAK_3401_repeater` | builds | 379,500 B (46.6% of 815,104) | 33,032 B (14.0% of 235,520) |
| `RAK_3401_hydra` (3 slots) | builds | 381,340 B (46.8%) | 54,336 B (23.1%) |
| `RAK_3401_hydra_debug` (packet trace on) | builds | 384,716 B (47.2%) | 65,288 B (27.7%) |
| `ThinkNode_M5_Repeater` | builds | 1,125,165 B (85.8% of 1,310,720) | 60,832 B (11.6% of 524,288) |

Cost of everything in this fork, measured against upstream `v1.17.1` built from the same
tree on the same host:

|  | upstream | this fork | delta |
|---|---:|---:|---:|
| `RAK_3401_repeater` flash | 376,780 B | 379,500 B | **+2,720 B** |
| `RAK_3401_repeater` RAM | 32,928 B | 33,032 B | **+104 B** |

The per-branch deltas sum to 2,752 (192 + 16 + 1,232 + 704 + 608); moving the LoRa
watchdog out of `MyMesh` into its own module then returned 32 bytes of flash and cost 24
of RAM.

The hydra figure is the one worth noting: the diagnostic packet trace is **10,952 B of
RAM and 3,376 B of flash**, and it is off unless a build asks for it. Sharing a radio used
to cost more than the identities sharing it did.

**What the M5 row does and does not mean.** `ThinkNode_M5_Repeater` is an unmodified
upstream target, and that is the whole claim: it still builds cleanly against our tree.
There is no M5-specific work here, no hydra env for M5, and none of the features above
are enabled on it. They are RAK3401-only so far.

**Builds verified is not tested on hardware.** No firmware from this tree has been
flashed to a board. Do not make a remote repeater your first flash target.

## Building

PlatformIO. The two added envs and one upstream env:

```bash
pio run -e RAK_3401_repeater      # stock repeater + watchdogs + status LED
pio run -e RAK_3401_hydra         # multi-identity node, 3 slots
pio run -e ThinkNode_M5_Repeater  # unmodified upstream target
```

Envs added by this fork: `RAK_3401_hydra`, and `native_multi` for the host tests. Every
other firmware env is upstream's, unchanged. `RAK_3401_repeater` is the one existing env
whose build flags changed — it gains `LOOP_WATCHDOG_MS`, `LORA_WATCHDOG_MS` and
`WITH_STATUS_LED`, which is trivially revertible.

Hydra is configured entirely from build flags in `variants/rak3401/platformio.ini`:
`HYDRA_NUM_SLOTS` (1..8), `HYDRA_NAME_PREFIX`, `HYDRA_CHAT_POOL`, `HYDRA_CHAT_HASHES`,
`MAX_CONTACTS`, `MAX_CONNECTIONS`.

### aarch64 hosts

PlatformIO's pinned ARM toolchain (GCC 7.2) has no `linux_aarch64` package, so nRF52 envs
need an override in `platformio.local.ini` (gitignored):

```ini
[env:RAK_3401_repeater]
platform_packages = platformio/toolchain-gccarmnoneeabi@~1.120301.0
[env:RAK_3401_hydra]
platform_packages = platformio/toolchain-gccarmnoneeabi@~1.120301.0
```

Override `platform_packages` only. Adding `extends` there clobbers the env's own `extends`
and breaks the build. ESP32 envs need no override.

## Tests

```bash
pio test -e native_multi -e native_multi_notrace -e native -e native_kiss_modem
```

174 cases, all passing. 126 of them are new here — `test_shared_radio` 86, `test_mux` 14,
`test_lora_watchdog` 18, and `test_shared_radio_notrace` 8 on its own env, since a second
binary is the only way to exercise a compile-time flag's off state. All run against mock
`mesh::Radio` and `Arduino` headers. The other 48 are upstream's.

This is the fork's only real safety net, and it is worth being plain about what it covers:
the arbiter's logic, not its behaviour on hardware.

## Relationship to upstream

This fork is meant to be **upstream-eligible**. That is a design constraint that shapes
the code, not a policy statement.

- **No changes to `src/Mesh.h`, `src/Dispatcher.h`, `src/Packet.h` or `src/MeshCore.h`.**
  Those four files are identical to `v1.17.1`. The `RadioPort : mesh::Radio` design means
  none have been needed, and that is the property being protected. Doing multi-identity by
  refactoring `Mesh` instead would mean multiplexing keys, ACLs, routing tables and prefs
  through code that every single-identity build also runs.
- **New behaviour arrives as new files plus a build flag**, never as an edited default:
  `LOOP_WATCHDOG_MS`, `LORA_WATCHDOG_MS`, `WITH_STATUS_LED`, `HYDRA_NUM_SLOTS`.
- **Anything that genuinely needs a core change gets raised as its own small upstream PR
  first**, rather than carried as a fork delta.
- **We rebase onto upstream periodically**, not just at PR time. The whole point of the
  constraints above is that the rebase should be nearly free.

The full diff, so you can judge that for yourself: 36 files, +6,729 / -96 lines against
`v1.17.1`, of which 2,425 lines are tests and mocks. Beyond the two bug fixes, the one
place this fork edits upstream code is `examples/simple_repeater/` — `MyMesh.{h,cpp}` and `main.cpp`, wiring up the watchdogs and the LED and moving the LoRa
watchdog's state machine out into its own module. Hydra itself needs
none of that; slot 0 reuses `MyMesh` as it stands.

## Credit

This is a fork, and the interesting part is not the fork.

MeshCore is [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore). The
protocol, the routing, the crypto, the packet format, the CLI, the board support across
dozens of targets, the flasher and the client apps are all the MeshCore community's work.
This tree is upstream `v1.17.1` with about 3,700 non-test lines added to it. The 8.7-10.4
KB of mesh protocol core that everything here depends on is theirs, and it is good code —
the reason `SharedRadio` could be written below `mesh::Radio` without touching `Mesh` or
`Dispatcher` at all is that upstream had already put the seam in the right place. That is
not luck; someone chose where the interface went.

On the two bugs specifically: `fix/packet-pool-locking` and `fix/acl-flash-writes` are
upstream bugs affecting everyone, not fork-local problems. They are already on their own
branches parented directly on `v1.17.1`, each one change, ready to submit — deliberately
kept that way rather than buried in a fork delta. The same offer stands for anything else
here upstream wants, hydra included.

The reliability work in this fork exists because two repeaters kept dying in ways nobody
noticed. That is a deployment problem, not a criticism of the firmware.

## Upstream documentation

Everything about MeshCore itself — hardware support, the flasher, the client apps, the
protocol docs, the FAQ — lives upstream:

- Documentation: <https://docs.meshcore.io>
- Flasher: <https://meshcore.io/flasher>
- Repeater and room-server config: <https://config.meshcore.io>
- Discord: <https://meshcore.gg>
- Repository and issue tracker: <https://github.com/meshcore-dev/MeshCore>

Report problems with what is described in this README here. Report anything else
upstream — and please check that it reproduces on stock upstream firmware first.

## License

MIT, same as upstream. See `license.txt`.
