# BLE bridge

Joins two MeshCore repeaters over Bluetooth so that nodes on **different LoRa
bands** behave as one mesh. A packet heard on one band is carried across and
retransmitted on the other. The pair ends up behaving roughly as though the two
radios were sitting next to each other.

The intended use is a site where one repeater covers a wide/slow band and
another a narrow/fast one, and you want traffic and adverts to flow between
them without a second radio or an internet link.

**nRF52840 only.** ESP32 repeaters cannot join a BLE bridge group.

---

## Quickstart

### 1. Build and flash

Use the bridge environment on both nodes:

```sh
pio run -e RAK_3401_repeater_bridge_ble -t upload
```

Over the air instead, once a node already runs a bridge build:

```sh
uv run --with bleak python tools/meshflash.py <ble-address> \
    .pio/build/RAK_3401_repeater_bridge_ble/firmware.zip \
    --expect-build "17 Aug 2026"
```

> **Both ends must run the same build.** The link's framing is part of the
> firmware, so a mismatched pair may fail to decode each other's frames.

### 2. Set a shared secret — do this first

Every node in a bridge group must share one secret. It keys an HMAC over each
frame, and it is the only thing separating your group from anyone else's.

```
set bridge.secret <something-long-and-private>
```

`bridge` reports `[DEFAULT SECRET - anyone can inject]` until you do. Do not
leave it at the default on anything you care about.

### 3. Enable the bridge

```
set bridge.enabled on
```

### 4. Verify

```
ble bridge
> ble on, PIN 343557 (new every boot), connected=no; slots 2p/2c mtu 123 q3

bridge
> ble bridge up: tx 114 | rx seen 1223 ok 77 hb 136 dup 9 bad 0/L5 other 1168 | peers 1

bridge links
> 1 link(s) up: EFD096/up/tx102/rx172/drop2
```

`peers 1` means the far node has been heard and authenticated. `links` shows the
point-to-point connection, which is where real traffic flows. Peers are
discovered by broadcast heartbeat every 15s, so allow a minute after boot.

### 5. Make sure both nodes have a clock

A node whose clock is unset stamps adverts with a date the rest of the mesh
rejects as replays, which makes it **invisible even when bridging works
perfectly** — you can be looking at healthy bridge counters on both sides and
still be unable to reach either node. These boards have no battery-backed RTC,
so this applies after **every reboot**, including every firmware update.

Check with `clock`. If it reads 2024, set it:

```
time <unix-epoch-seconds>
```

---

## Key settings

| Setting | Default | What it does |
|---|---|---|
| `bridge.enabled` | `off` | Master switch. |
| `bridge.secret` | *(unset)* | **Set this.** Shared HMAC key for the group. |
| `bridge.source` | `both` | Which packets cross: `tx`, `rx`, or `both`. See below. |
| `bridge.adv_repeat` | `3` | Times each broadcast datagram is repeated. Broadcast is unacknowledged; this is the only redundancy it has. |
| `bridge.scan_duty` | `50` | Percent of each scan cycle spent listening. |
| `bridge.scan_filter` | `off` | Link-layer filter to known peers. Cuts foreign advert wakeups; costs discovery of new peers outside the periodic window. |
| `bridge.ble_hold` | `0` | Milliseconds to hold a datagram before transmitting, to avoid keying both radios at once on 1W nodes. |
| `bridge.delay` | `0` | Milliseconds a bridged packet waits before entering the mesh. |

### `bridge.source` — use `both`

This is the setting most likely to cause confusing behaviour if changed.

- **`rx`** forwards what this node *hears*. Needed because our routing policy
  drops duplicates and hop-exceeded packets as old news **on this band**, while
  the far band may never have seen them at all. Dedup is per-band; the bridge
  crosses bands.
- **`tx`** forwards what this node *transmits*, which includes its own adverts
  and replies. Without it the node can be addressed from the far side but its
  answers never come back.
- **`both`** is the default and the only setting that is actually a bridge.

Loop-safe: a relayed packet echoing back to its origin is dropped by the
duplicate table at the far end. One wasted crossing, no loop.

### Build-time flags

| Flag | Purpose |
|---|---|
| `WITH_BLE_BRIDGE=1` | Compiles the bridge in. |
| `WITH_BLE_CLI=1` | BLE CLI and DFU. On a node without a cable this is the only way in — keep it. |
| `BLE_PRPH_SLOTS` | Inbound connections. **2 minimum**: one for CLI/DFU, one for a peer dialling in. At 1 they evict each other. |
| `BLE_CENTRAL_SLOTS` | Outbound links. 1 is enough for a two-node bridge. |
| `LOOP_WATCHDOG_MS` | Reboots the node if the main loop stalls this long. |

---

## Reading the diagnostics

```
bridge
> ble bridge up: tx N | rx seen N ok N hb N dup N bad N/LN other N | peers N
```

| Field | Meaning |
|---|---|
| `ok` | Mesh packets accepted from the bridge. |
| `hb` | Peer heartbeats (every 15s). |
| `dup` | Already-seen packets dropped — this is loop prevention working, not an error. |
| `bad N/LN` | Failed HMAC: **broadcast / link**. See below. |
| `other` | BLE adverts that are not our protocol. Ambient noise; ignore. |

**`bad` is split by transport because the halves mean opposite things.** A
broadcast failure is expected background — company ID `0xFFFF` is the SIG's
shared development ID, so anyone's beacon reaches the check, as does a
neighbouring bridge group with a different secret. A **link** failure is a bug:
those bytes crossed an acknowledged connection. A rising `L` count means framing
trouble, not interference.

```
bridge links   per-connection state, frames sent/received, drops
bridge peers   RSSI, packets, age, clock skew, broadcast loss
bridge cpu     ingest cost, loop rate, worst loop gap, scanner recoveries
bridge cpu reset   zero the loop statistics
```

`bridge peers` reports `loss` for the **broadcast** heartbeat path only. It is
normally high and that is expected — broadcast is unacknowledged. Real traffic
uses the link, which retries in the controller; judge that by `bridge links`.

`ble bridge` reports `mtu` and `q`. These are negotiated at boot against the
SoftDevice's RAM budget, and connection slots are held in preference to buffers.
`mtu 23 q1` means the node is short on RAM and fragmenting every frame.

---

## Gotchas

- **Both ends need the same secret and the same build.**
- **`bridge.enabled off` disconnects the links.** It has to: a link left
  connected occupies a peripheral slot on the *far* node, and a node with no
  free slot stops advertising, which makes it unreachable.
- **A busy bridge does not stop you reaching a node.** The connectable advert is
  maintained independently, and the node advertises whenever a peripheral slot
  is free — which is why `BLE_PRPH_SLOTS` should be at least 2.
- **Broadcast loss is not link loss.** Expect tens of percent on the heartbeat
  path; the link is what carries traffic.
- **Flash writes stall the main loop for around 1.6 seconds** while the
  SoftDevice arbitrates. Normal operation performs none — only CLI settings, a
  new ACL client, or a permission change.
