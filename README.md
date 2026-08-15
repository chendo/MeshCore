# BLE Bridging (nRF52)

> This branch adds a **BLE bridge** for nRF52840 repeaters. It is the nRF52
> counterpart to `WITH_ESPNOW_BRIDGE`: two or more co-located repeaters share
> every packet they transmit, so traffic crosses between them without spending a
> LoRa hop. Boards like the RAK3401 have no WiFi, so ESP-NOW is unavailable to
> them and BLE is the only 2.4 GHz radio they have.

## What it is for

Run two repeaters at one site on **different radio configs** — say one on
`Australia (Narrow)` and one on `Australia (Mid)` — and give them the same
`bridge.secret`. Each one re-floods onto its own band whatever the other heard.
The two bands become one mesh, without either repeater having to hear the other
over LoRa at all.

## How it works

Each node broadcasts every packet it transmits as a **BLE 5 extended
advertisement**, and scans continuously for its peers' broadcasts. There is no
pairing, no connection and no peer state: every node in range hears every
datagram, exactly like ESP-NOW.

Extended advertising matters. A legacy advert carries 31 bytes, of which about 24
would survive framing, so a full MeshCore packet would need eleven fragments with
no acknowledgement anywhere — delivery decays as `(1-p)^11` and collapses the
moment two nodes talk at once. An extended PDU carries 255, so a packet always
fits one transmission and there is no reassembly to get wrong. That leaves **238
bytes** for the packet, against ESP-NOW's 246.

One advertisement carries one frame:

```text
[1]     version
[4]     timestamp, little-endian, sender's clock
[<=238] the mesh packet, IN PLAINTEXT
[8]     HMAC-SHA256 tag over everything above, truncated
```

**The payload is deliberately plaintext.** ESP-NOW's bridge XORs its payload with
the shared secret, which its own header admits is not encryption. Mesh traffic is
already public over the air, so pretending otherwise buys nothing and makes the
format harder to implement independently. Message *contents* stay protected by
MeshCore's own end-to-end encryption, exactly as they are over LoRa.

**Authentication replaces the checksum.** Because the format is open, anything in
radio range could otherwise inject packets straight into your mesh. The truncated
HMAC replaces both the Fletcher-16 checksum and the XOR: a frame keyed with a
different secret fails the tag check, which is precisely the network-isolation
role ESP-NOW's post-encryption checksum plays. Eight tag bytes put a blind
forgery at 2⁻⁶⁴ per attempt, from inside radio range.

Received frames are queued as if they had arrived over the air, so routing,
dedup, and admin handling are unchanged. Loops are prevented because MeshCore's
packet hash ignores the path: a packet that returns over the bridge with an extra
hop hashes identically to the one already seen, and is dropped.

## Quickstart

**1. Build and flash** — the reference target is the RAK3401:

```bash
pio run -e RAK_3401_repeater_bridge_ble -t upload --upload-port /dev/ttyACM0
```

For another nRF52840 board, add `-D WITH_BLE_BRIDGE=1` to its env plus these to
`build_src_filter`:

```ini
  +<helpers/nrf52/BleBroadcast.cpp>
  +<helpers/bridges/BridgeBase.cpp>
  +<helpers/bridges/BLEBridge.cpp>
```

**2. Set the same secret on every node in the group** — over the serial console:

```text
set bridge.secret YourRandomSecretHere
```

> ⚠️ **Change it.** The secret is not optional — every frame is tagged and every
> frame is checked — but the factory default is published in this source, so a
> node still carrying it will accept packets from anyone in radio range. `bridge`
> flags this as `[DEFAULT SECRET - anyone can inject]` until you change it. It is
> a credential, not a cipher: anyone holding it can forge frames, so prefer a
> random string, and note the field holds 15 characters.

**3. Check it is working:**

```text
> bridge
ble bridge up: tx 24 drop 0 | rx seen 102 ok 12 dup 8 bad 0 other 82 | peers 1
> bridge peers
1 bridge peer(s): EFD096/-33dB/17pkt/33s/skew+4076s
```

`ok` climbing means frames are being accepted from a peer. Two counters are easy
to misread: **`dup`** is healthy — each datagram is deliberately broadcast over
several advertising events so a duty-cycled scanner cannot miss it — and
**`other`** is ambient noise, because `0xFFFF` is the Bluetooth SIG's shared
development company ID and other people's beacons land there too. A climbing
**`bad`** means another group is in range on a different secret, which is the
isolation working.

## Limits worth knowing

- **BLE range is tens of metres.** This is a same-site link, not a backhaul.
- **nRF52 only.** ESP32 repeaters cannot join a BLE bridge group.
- **The SoftDevice has one advertising set**, so the bridge time-shares it with
  the BLE diagnostic/DFU port if the build has one. At least 300 ms of every 2 s
  is reserved for connectable advertising, so that port stays discoverable, but
  finding it can take longer while the bridge is busy.
- **`set bridge.enabled off`** stops the bridge, including its continuous
  scanner, which is the dominant power draw. Measured on a RAK3401 the whole BLE
  stack costs roughly 10 mW against a ~110 mW idle baseline.

See `docs/cli_commands.md` for the full command reference.

---

## About MeshCore

MeshCore is a lightweight, portable C++ library that enables multi-hop packet routing for embedded projects using LoRa and other packet radios. It is designed for developers who want to create resilient, decentralized communication networks that work without the internet.

## 🔍 What is MeshCore?

MeshCore now supports a range of LoRa devices, allowing for easy flashing without the need to compile firmware manually. Users can flash a pre-built binary using tools like Adafruit ESPTool and interact with the network through a serial console.
MeshCore provides the ability to create wireless mesh networks, similar to Meshtastic and Reticulum but with a focus on lightweight multi-hop packet routing for embedded projects. Unlike Meshtastic, which is tailored for casual LoRa communication, or Reticulum, which offers advanced networking, MeshCore balances simplicity with scalability, making it ideal for custom embedded solutions, where devices (nodes) can communicate over long distances by relaying messages through intermediate nodes. This is especially useful in off-grid, emergency, or tactical situations where traditional communication infrastructure is unavailable.

## ⚡ Key Features

* Multi-Hop Packet Routing
  * Devices can forward messages across multiple nodes, extending range beyond a single radio's reach.
  * Supports up to a configurable number of hops to balance network efficiency and prevent excessive traffic.
  * Nodes use fixed roles where "Companion" nodes are not repeating messages at all to prevent adverse routing paths from being used.
* Supports LoRa Radios – Works with Heltec, RAK Wireless, and other LoRa-based hardware.
* Decentralized & Resilient – No central server or internet required; the network is self-healing.
* Low Power Consumption – Ideal for battery-powered or solar-powered devices.
* Simple to Deploy – Pre-built example applications make it easy to get started.

## 🎯 What Can You Use MeshCore For?

* Off-Grid Communication: Stay connected even in remote areas.
* Emergency Response & Disaster Recovery: Set up instant networks where infrastructure is down.
* Outdoor Activities: Hiking, camping, and adventure racing communication.
* Tactical & Security Applications: Military, law enforcement, and private security use cases.
* IoT & Sensor Networks: Collect data from remote sensors and relay it back to a central location.

## 🚀 How to Get Started

- Watch the [MeshCore QuickStart Playlist](https://www.youtube.com/watch?v=iaFltojJrAc&list=PLshzThxhw4O4WU_iZo3NmNZOv6KMrUuF9) by The Comms Channel
- Watch the [MeshCore Technical Presentation](https://www.youtube.com/watch?v=OwmkVkZQTf4) by Liam Cottle.
- Read through our [Frequently Asked Questions](./docs/faq.md) and [Documentation](https://docs.meshcore.io).
- Flash the MeshCore firmware on a supported device.
- Connect with a supported client.

For developers:

- Install [PlatformIO](https://docs.platformio.org) in [Visual Studio Code](https://code.visualstudio.com).
- Clone and open the MeshCore repository in Visual Studio Code.
- See the example applications you can modify and run:
  - [Companion Radio](./examples/companion_radio) - For use with an external chat app, over BLE, USB or Wi-Fi.
  - [KISS Modem](./examples/kiss_modem) - Serial KISS protocol bridge for host applications. ([protocol docs](./docs/kiss_modem_protocol.md))
  - [Simple Repeater](./examples/simple_repeater) - Extends network coverage by relaying messages.
  - [Simple Room Server](./examples/simple_room_server) - A simple BBS server for shared Posts.
  - [Simple Secure Chat](./examples/simple_secure_chat) - Secure terminal based text communication between devices.
  - [Simple Sensor](./examples/simple_sensor) - Remote sensor node with telemetry and alerting.

The Simple Secure Chat example can be interacted with through the Serial Monitor in Visual Studio Code, or with a Serial USB Terminal on Android.

## ⚡️ MeshCore Flasher

We have prebuilt firmware ready to flash on supported devices.

- Launch https://meshcore.io/flasher
- Select a supported device
- Flash one of the firmware types:
  - Companion, Repeater or Room Server
- Once flashing is complete, you can connect with one of the MeshCore clients below.

## 📱 MeshCore Clients

**Companion Firmware**

The companion firmware can be connected to via BLE, USB or Wi-Fi depending on the firmware type you flashed.

- Web: https://app.meshcore.nz
- Android: https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android
- iOS: https://apps.apple.com/us/app/meshcore/id6742354151?platform=iphone
- NodeJS: https://github.com/liamcottle/meshcore.js
- Python: https://github.com/fdlamotte/meshcore-cli

**Repeater and Room Server Firmware**

The repeater and room server firmware can be set up via USB in the web config tool.

- https://config.meshcore.io

They can also be managed via LoRa in the mobile app by using the Remote Management feature.

## 🛠 Hardware Compatibility

MeshCore is designed for devices listed in the [MeshCore Flasher](https://meshcore.io/flasher)

## 📜 License

MeshCore is open-source software released under the MIT License. You are free to use, modify, and distribute it for personal and commercial projects.

## Contributing

Please submit PR's using 'dev' as the base branch!
For minor changes just submit your PR and we'll try to review it, but for anything more 'impactful' please open an Issue first and start a discussion. It is better to sound out what it is you want to achieve first, and try to come to a consensus on what the best approach is, especially when it impacts the structure or architecture of this codebase.

Here are some general principles you should try to adhere to:
* Keep it simple. Please, don't think like a high-level lang programmer. Think embedded, and keep code concise, without any unnecessary layers.
* No dynamic memory allocation, except during setup/begin functions.
* Use the same brace and indenting style that's in the core source modules. (A .clang-format is probably going to be added soon, but please do NOT retroactively re-format existing code. This just creates unnecessary diffs that make finding problems harder)

Help us prioritize! Please react with thumbs-up to issues/PRs you care about most. We look at reaction counts when planning work.

### Running unit tests

To run unit tests, run the following command:

```bash
pio test --environment native --verbose
```

## Road-Map / To-Do

There are a number of fairly major features in the pipeline, with no particular time-frames attached yet. In very rough chronological order:
- [X] Companion radio: UI redesign
- [X] Repeater + Room Server: add ACL's (like Sensor Node has)
- [X] Standardise Bridge mode for repeaters
- [ ] Repeater/Bridge: Standardise the Transport Codes for zoning/filtering
- [X] Core + Repeater: enhanced zero-hop neighbour discovery
- [ ] Core: round-trip manual path support
- [ ] Companion + Apps: support for multiple sub-meshes (and 'off-grid' client repeat mode)
- [ ] Core + Apps: support for LZW message compression
- [ ] Core: dynamic CR (Coding Rate) for weak vs strong hops
- [ ] Core: new framework for hosting multiple virtual nodes on one physical device
- [ ] V2 protocol spec: discussion and consensus around V2 packet protocol, including path hashes, new encryption specs, etc

## 📞 Get Support

- Report bugs and request features on the [GitHub Issues](https://github.com/ripplebiz/MeshCore/issues) page.
- Find additional guides and components on [my site](https://buymeacoffee.com/ripplebiz).
- Join [MeshCore Discord](https://meshcore.gg) to chat with the developers and get help from the community.
