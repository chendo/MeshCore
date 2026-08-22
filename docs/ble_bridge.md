# BLE bridge

Joins two MeshCore repeaters over Bluetooth, so that nodes on **different LoRa
bands** behave as one mesh. A packet that one node hears is carried across the
BLE link and retransmitted on the other band. The pair then behaves roughly as
though the two radios sat next to each other.

The intended use is a site with one repeater on a wide and slow band and another
on a narrow and fast one, where traffic and adverts must flow between them with
no second radio and no internet link.

This is the nRF52 counterpart to the ESP-NOW bridge. A board such as the RAK3401
or the Elecrow ThinkNode M1 has no WiFi, so ESP-NOW is unavailable to it.

**nRF52840 only.** An ESP32 repeater cannot join a BLE bridge group.

---

## How it works

Three layers, each in its own file under `src/helpers/`:

| Layer | File | Job |
|---|---|---|
| Stack | `nrf52/BleStack.{h,cpp}` | Starts the SoftDevice with the connection roles that a bridge needs, and steps down the buffer tier before it gives up a connection slot. |
| Discovery | `nrf52/BleDiscovery.{h,cpp}` | Advertises a beacon that names the group, and scans for the same beacon from other nodes. |
| Link | `nrf52/BleLink.{h,cpp}` | Opens the connection and carries the frames. |
| Bridge | `bridges/BLEBridge.{h,cpp}` | Builds and checks the frames, and hands packets to the mesh. |
| Frame | `bridges/BleBridgeFrame.h` | The wire format, the group marker and the deny list. Header only, so the host tests run it. |

### The transport is a connection, not a broadcast

An earlier version of this bridge sent each frame as a BLE 5 extended advert.
That cannot be made reliable. Measured on a live pair, a single advert lands
about half of the time; to send each frame three to five times moves the loss
from 49% to 3.4%, and never to zero, because nothing acknowledges anything. CPU,
the central connection, our own transmissions and foreign advert traffic were
each excluded by measurement. What remains is intrinsic to extended
advertising.

A connection is different in kind. The link layer acknowledges every packet and
retransmits it until the peer receives it or the supervision timeout ends. Two
more gains follow:

- A frame is up to 256 bytes, so a full MeshCore packet fits. The advert path
  carried 236 bytes and discarded a packet with a long path in silence.
- Fragmentation over a connection is routine, because the peer acknowledges each
  fragment. Over a broadcast it was fatal.

### Discovery and trust

Each node advertises a legacy beacon, in one Manufacturer Specific Data AD
structure:

```
[2] company ID (0xFFFF, the SIG development ID)
[1] record version
[2] group marker
[1] battery, in decivolts
```

The **group marker** is two bytes derived from `bridge.secret` over a fixed
label. It is what stops a node from dialling every MeshCore device in range,
which matters because there are only three central slots.

The marker is PUBLIC. Anyone in radio range can read it from a beacon and repeat
it, so it filters and it does not authenticate. Proof of group membership is the
group HMAC on the **first frame over the link**.

A stranger can therefore make a node open a connection, and it can also dial the
node itself. It gets no data, and the rules are the same in both directions:

- a link whose first frame fails the group tag is dropped at once;
- a link that never authenticates is dropped after 45 seconds, whether it wrote
  something or nothing at all;
- a link that authenticates and then goes silent is dropped after 60 seconds;
- either way the address goes on a deny list for 5 minutes, and that list is
  checked both before the node dials out and before it adopts a peer that
  dialled in.

That deny list is what stops a repeat attacker from cycling the three central
slots and the one inbound slot. What an attacker in radio range can still do is
occupy a slot for a few seconds at a time, from a fresh address each time, and
read the group marker. It cannot inject a packet into the mesh, and it cannot
read the secret.

The second rule used to have a hole. The bridge saw a peer that dialled in only
on its FIRST write, because that write was the event it hooked. A peer that
connected inward and then sent nothing was invisible, so it was never adopted,
never timed out and never denied, and it held the inbound peripheral slot for as
long as the connection lived. `BleLink::loop()` now sweeps the peripheral
connections on every main pass and adopts a peer that has written nothing. The
authentication grace, the idle limit and the deny list then all reach it.

The sweep is a poll, and it registers NO Bluefruit callback.
`Periph.setConnectCallback` is a single slot that `SerialBLEInterface` already
holds, and a second owner would take it from the first in silence.
`Bluefruit.Periph.connected(handle)` reports the role and the liveness of one
connection together, so an outward link of our own never matches the sweep.

One restriction remains, and it is a restriction and not a hole. The sweep skips
a connection that is secured or bonded. The CLI and DFU both need MITM
encryption before they carry a byte, and a bridge peer never pairs, so that test
is exact for a CLI session in use. It is not exact for a CLI client that sits at
the passkey prompt: that client is not secured yet, and the sweep cannot tell it
from a peer that says nothing. A build that carries the bridge AND the BLE CLI
must therefore set `-D BLE_LINK_SILENT_SWEEP=0`. Such a build keeps the old
fault: a silent inbound peer holds the slot until its connection ends. No
shipped env carries both today.

Only the node with the numerically lower BLE address dials. A pair therefore
agrees on exactly one link, with no negotiation and no timers.

### The frame

```
[1]     version
[2]     sequence, little-endian
[4]     timestamp, little-endian, by the sender's clock
[<=241] the mesh packet, IN PLAINTEXT
[8]     HMAC-SHA256 over everything above, truncated
```

The frame is plaintext on purpose. Mesh traffic is already public over the air,
so to obfuscate it buys nothing and makes the format harder to implement
independently. The truncated HMAC replaces both the checksum and the XOR that
the ESP-NOW bridge uses, and it plays the same network-isolation role: a frame
that carries a different secret fails the tag check.

Eight tag bytes put a blind forgery at 2^-64 for each attempt, and the attacker
must be in radio range to try. MeshCore ships two-byte MACs on encrypted direct
messages, so this is four times the protocol's own norm.

An idle link carries a heartbeat every 15 seconds. Without it a link with no
packets to carry looks the same as a dead one, because the supervision timeout
notices a radio that went away and not a peer that stopped to talk.

---

## Quickstart

### 1. Build and flash both nodes

```sh
pio run -e RAK_3401_repeater_bridge_ble -t upload
pio run -e ThinkNode_M1_repeater_bridge_ble -t upload   # the ThinkNode M1
```

Both envs belong to this fork and are defined in `variants/hydra_rak3401` and
`variants/hydra_m1`. The board definitions under `variants/rak3401` and
`variants/thinknode_m1` are upstream's and carry no configuration of ours.

Both ends must run the same build. The framing is part of the firmware.

### 2. Set a shared secret, and do this first

Every node in a bridge group shares one secret. It keys the HMAC over each
frame, and it is the only thing that separates your group from anyone else's. It
also produces the group marker, so two nodes with different secrets never dial
each other.

```
set bridge.secret <something-long-and-private>
```

It is a credential and not a cipher: anyone who holds it can forge frames.
Prefer a random string, because its entropy caps the entropy of the key.

### 3. Enable the bridge

```
set bridge.enabled on
```

### 4. Give both nodes a clock

A node whose clock is unset stamps its adverts with a date that the rest of the
mesh rejects as a replay. It is then **invisible even while the bridge works
perfectly**: the counters look healthy on both sides and neither node can be
reached. These boards have no battery-backed RTC, so this applies after every
reboot, including every firmware update.

Check with `clock`. If it reads 2024, set it:

```
time <unix-epoch-seconds>
```

---

## Settings

| Setting | What it does |
|---|---|
| `bridge.enabled` | Master switch. |
| `bridge.secret` | **Set this.** The shared HMAC key for the group. |
| `bridge.source` | Which packets cross: `logTx` (0) or `logRx` (1). |
| `bridge.delay` | Milliseconds that a bridged packet waits before it enters the mesh. |

`bridge.type` reports `ble` on this build. The transport type code in the
repeater status reply is `0x04`; `0x01` is UART and `0x03` is ESP-NOW.

### Build-time flags

| Flag | Purpose |
|---|---|
| `${bridge.ble}` | Selects this bridge. Put it in the env's `build_flags`. |
| `BLE_PRPH_SLOTS` | Inbound connections. **2 minimum**: one for a peer that dials in, one spare. |
| `BLE_CENTRAL_SLOTS` | Outward links. 1 is enough for a two-node bridge. |
| `BLE_LINK_SILENT_SWEEP` | Reclaims the inbound slot from a peer that connects and writes nothing. On by default. Set it to **0** in a build that also carries the BLE CLI. |
| `LOOP_WATCHDOG_MS` | Reboots the node if the main loop stalls this long. |
| `BRIDGE_DEBUG` | Serial log for each frame. Off in a shipped build. |
| `BLE_DISCOVERY_DEBUG_LOGGING` | Serial log from the advert arbiter and the scanner. |

The env also needs these in `build_src_filter`:

```
+<helpers/nrf52/BleStack.cpp>
+<helpers/nrf52/BleDiscovery.cpp>
+<helpers/nrf52/BleLink.cpp>
+<helpers/bridges/BLEBridge.cpp>
```

---

## Do not add a BLE CLI to a bridge build

This bridge owns the BLE stack. It drives advertising set 0 directly, because
the SoftDevice has exactly one advertising set and the node needs both a
connectable advert and a beacon on it. A second owner of that set fights it, and
the node then stops to advertise while it bridges perfectly well — which leaves
a repeater with no cable unreachable.

The node still advertises as connectable whenever a peripheral slot is free, so
a phone or laptop scanner sees it and DFU still works. When no slot is free the
node falls back to a non-connectable beacon, so it stays visible even though it
cannot be connected to.

---

## Cost

Measured against `RAK_3401_repeater_hardened` on the same tree:

| | Flash | RAM |
|---|---|---|
| `RAK_3401_repeater_hardened` | 380,092 | 33,048 |
| `RAK_3401_repeater_bridge_ble` | 395,848 | 41,200 |
| **Cost of the bridge** | **+15,756** | **+8,152** |

The same pair on the ThinkNode M1, which carries the same nRF52840 and the same
SoftDevice:

| | Flash | RAM |
|---|---|---|
| `ThinkNode_M1_repeater_hardened` | 304,848 | 30,768 |
| `ThinkNode_M1_repeater_bridge_ble` | 320,412 | 38,904 |
| **Cost of the bridge** | **+15,564** | **+8,136** |

For comparison, the two-transport experiment on `origin/ble-clean`, which also
carried datagrams over extended advertising, measured +28,940 flash and +14,712
RAM.

The RAM figure is static allocation only. The SoftDevice takes its own 24KB,
which the linker reserves whether or not this bridge is compiled in.

---

## Tests

`test/test_ble_bridge/` covers two parts. `BleBridgeFrame.h` is header only and
free of the BLE stack: the frame layout and its tag, the beacon record and the
group marker, the deny list, and the oversize guard with its counter.

`BleLink.cpp` builds against a Bluefruit stand-in in `test/mocks_ble_link/`, so
the shipped source file is what runs. That covers the framing and the teardown
paths: a header split across two writes, a truncated frame, a hunt that lands on
a payload byte that looks like SYNC, a reconnect with a part-drained queue, and
the deny list and the silence limit on the inbound peer. The stand-in also
presents a peripheral connection, so the sweep has cases of its own: a peer that
writes nothing loses the slot and reaches the deny list, a peer that writes in
time keeps it, a peer dropped for silence is not adopted again at once, and a
paired connection is left to the CLI.

`BleStack`, `BleDiscovery` and `BLEBridge` itself still need a SoftDevice and
have no host test.

```sh
pio test -e native_ble
```
