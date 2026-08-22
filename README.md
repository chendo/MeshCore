# MeshCore-Hydra

A fork of [MeshCore](https://github.com/meshcore-dev/MeshCore). It adds three things:
work that keeps a node alive, a node role that carries several mesh identities, and a
Bluetooth bridge. It stays close to upstream on purpose.

The base is upstream `v1.17.1` (`d9296435`). Almost all of the new code is in new files
behind new build flags. This tree also changes 20 upstream source files. Two of them hold
bug fixes. Seven carry the link-metric work. We offer both of those back to upstream.
[docs/hydra.md](docs/hydra.md) names every upstream file that this fork changes.

No firmware from this tree has run on real hardware. Only host tests and builds support
the claims here. The new features build for the RAK3401 (nRF52840) and for no other
board. The room protocol is not written yet. Read [docs/hydra.md](docs/hydra.md) before
you flash a node.

## Key features

**Multi-identity nodes.** One board and one LoRa radio carry up to 8 mesh identities.
Each identity has its own keypair, its own name and its own contacts. Slot 0 is the
repeater. Slots 1 and above are chat identities or room servers. An arbiter below
`mesh::Radio` shares the transceiver and pools the duty cycle across all of them.

**Bluetooth bridges.** Two nRF52840 nodes at the same site open a Bluetooth link
between them. They pass mesh traffic across that link. The traffic does not use a LoRa
hop, and the site needs no second radio and no internet link. See
[docs/ble_bridge.md](docs/ble_bridge.md).

**Better link metrics.** Each received packet carries its own RSSI and its own coding
rate, and not only the SNR. The radio driver counts each receive failure against its
cause: a bad CRC, a damaged header, a timeout, or something else. A passive observer
reads the path of each packet and builds a map of which node hears which node.

## Build and flash

The project uses PlatformIO.

```bash
pio run -e RAK_3401_repeater             # repeater, watchdogs, status LED
pio run -e RAK_3401_hydra                # multi-identity node, 3 slots
pio run -e RAK_3401_repeater_bridge_ble  # repeater with a Bluetooth bridge
```

To flash a board, connect it over USB and add the upload target:

```bash
pio run -e RAK_3401_hydra -t upload
```

Then open the serial console at 115200 baud and type `help`.

These envs need a RAK3401 (nRF52840). This fork adds one more, `RAK_3401_hydra_debug`,
which turns the packet trace on. The other firmware envs are upstream's. This tree
changes only how a bridged env names its bridge class.

On an aarch64 Linux host, the pinned ARM toolchain has no package. Write a
`platformio.local.ini` file with one section for each env that you build:

```ini
[env:RAK_3401_hydra]
platform_packages = platformio/toolchain-gccarmnoneeabi@~1.120301.0
```

Set `platform_packages` only. Do not set `extends` there. It replaces the `extends` of
the env and the build fails. ESP32 envs need no override.

## The hydra CLI

The hydra node answers the standard repeater CLI. A command with no `slot N` prefix goes
to slot 0. These commands are new.

### Node commands

| Command | Result |
|---|---|
| `slots` | Show each slot: type, name, state, key prefix, packet counts and free RAM. |
| `peers` | Show the peer table of the observer. Console only. |
| `stats-shared` | Show the counters of the radio arbiter for the whole node. |
| `stats-txwait` | Show why the node could not transmit. Eight causes. |
| `trace` | Print the packet trace to the console. Build with `PKT_TRACE_ENTRIES` first. |

### Slot commands

Give the slot number, from 1 to `HYDRA_NUM_SLOTS` minus 1. `slot 0 <command>` also
works, and it goes to the repeater CLI.

| Command | Result |
|---|---|
| `slot N set name <text>` | Name the slot. Do this first. |
| `slot N on` / `slot N off` | Start or stop the slot. |
| `slot N chat` | Start the slot as a chat identity. |
| `slot N room` | Start the slot as a room server. |
| `slot N set advert.interval <mins>` | The advert period in minutes. Use 0 for never, or 60 to 240. |
| `slot N set flood.advert on\|off` | Send each scheduled advert as a flood. |
| `slot N set prv.key <hex>` | Move an identity that exists onto the slot. |
| `slot N get name\|type\|advert.interval\|flood.advert\|public.key` | Read one setting. |
| `slot N get prv.key` | Read the private key. Console only. |
| `slot N advert` / `slot N flood advert` | Send one advert now. |
| `slot N contacts` | Count the contacts of the slot. |
| `slot N setperm <pubkey-hex> <perms>` | Give a client a permission. Room slots. |
| `slot N clear acl` | Empty the room ACL. Room slots. |
| `slot N get acl` | List the room ACL. Room slots, console only. |
| `slot N posts` | Count the posts that the room holds. Room slots. |

### The rules

- **A slot needs a name first.** `slot 1 on` fails until you run `slot 1 set name`. An
  identity with no name must never advertise.
- **Node-level commands own the shared radio.** A slot refuses them. There is one
  transceiver, one board and one clock, so these settings belong to the whole node. Use
  them with no slot prefix. Examples are `freq`, `bw`, `sf`, `cr`, `tx`, `cad`,
  `dutycycle`, `time`, `password` and `reboot`.
- **Slot 0 is the repeater.** It is always on. You cannot stop it or change its type.
- **A slot keeps the type that it started with** until the next reboot. To change the
  type of a slot that ran, reboot the node first.
- `peers` and `get prv.key` work on the serial console only. They never answer a remote
  command. `peers` describes third parties, and the private key is the identity.

## More documentation

- [docs/hydra.md](docs/hydra.md) — what each feature does, the measured sizes, the
  build results, the limits, and the relationship with upstream.
- [docs/ble_bridge.md](docs/ble_bridge.md) — the Bluetooth bridge: the wire format, the
  settings and the threat model.

Everything about MeshCore itself is upstream:
<https://docs.meshcore.io>, <https://meshcore.io/flasher>, <https://meshcore.gg>.

## Credit

MeshCore is the work of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore)
and its community. **Theirs** is the protocol, the routes, the cryptography, the packet
format, the CLI, the board support for dozens of targets, the flasher and the client
apps. This tree is upstream `v1.17.1` with 31 new source files added to it.

**Ours** is a short list: `SharedRadio`, the hydra node role in `examples/hydra/`,
`MeshObserver`, the Bluetooth bridge, `LoraWatchdog`, `StatusLed`, `LoopWatchdog`,
`I2CBusRecovery`, and a `MuxSerialInterface` that no build compiles yet. We also changed
20 upstream files: the two bug fixes, the link metrics, the changes in
`examples/simple_repeater/`, and the choice of a bridge class by a build flag.
[docs/hydra.md](docs/hydra.md) names each one.

`SharedRadio` sits below `mesh::Radio`. It changes no line of the `Mesh` class and no
line of the `Dispatcher` class. That is possible because upstream had already put the
interface in the correct place. Someone chose where that seam went, and they chose
well.

We offer our fixes back rather than keep them. Two upstream bugs, a packet-pool race and
a flash write on each login, sit on their own branches on top of `v1.17.1`, one change
each, ready to send. The two small additions to `Dispatcher.h` and `Packet.h` follow the
same route. The same offer covers anything else here that upstream wants, and it covers
hydra.

Report a problem with what this README describes here. Report anything else upstream,
and please first make sure that it happens on stock upstream firmware.

## License

MIT, the same as upstream. See `license.txt`.
