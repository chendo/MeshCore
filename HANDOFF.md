# MeshCore multi-identity (ThinkNode M5) — local agent handoff

You are taking over a firmware project mid-build. It was developed in a cloud
sandbox that has no USB access to the hardware; you're running locally on the
machine the ThinkNode M5 is plugged into, so your job is to build, flash, and
iterate on real hardware. Read this whole brief before doing anything.

## What this firmware is

One ESP32-S3 (ThinkNode M5, 4 MB flash, 8 MB octal PSRAM) running **three
independent MeshCore identities at once, sharing the single LoRa radio**:

1. **repeater** — stock `examples/simple_repeater`, public, shares its location
2. **room**     — stock `examples/simple_room_server`, location suppressed
3. **companion**— stock `examples/companion_radio` (BaseChatMesh), location off

The three stock example meshes are compiled **UNMODIFIED**. They are pulled in
through tiny wrapper translation units that rename each `MyMesh` class and
`#include` the stock `.cpp` verbatim. A custom composition root owns the board,
the radio, WiFi, and a single HTTPS web panel, and shares the one radio between
the three meshes via a `SharedRadioCore` arbiter (RX fan-out + TX serialization).

Base is the EastMesh fork of MeshCore (upstream MeshCore v1.16.0 protocol),
commit `d350934`. Everything we added is on top of that, on branch
`multi-identity`.

## Get the code

Preferred: extract the tarball you were given.

    tar xzf meshcore-multi.tar.gz
    cd meshcore-multi

Alternative (reconstruct from upstream + patch):

    git clone https://github.com/xJARiD/MeshCore-EastMesh meshcore-multi
    cd meshcore-multi
    git checkout d350934
    git checkout -b multi-identity
    git am < meshcore-multi.patch      # or: git apply meshcore-multi.patch

## Toolchain

PlatformIO (the project pins its platform/toolchain):

    pip install platformio esptool
    # first build downloads the esp32-s3 toolchain (~a few min)

## Build

    pio run -e ThinkNode_M5_multi

Last known-good build: links clean, **flash ~65% of the 2.36 MB app slot**,
RAM ~11%. A merged flashable image is produced with:

    pio run -e ThinkNode_M5_multi -t mergebin
    # -> .pio/build/ThinkNode_M5_multi/firmware-merged.bin   (write at 0x0)
    # plain app image (fast reflash, same layout): .pio/build/.../firmware.bin  (write at 0x10000)

## Flash

Use the included `flash_multi.sh`, or esptool directly. The ESP32-S3 has native
USB; a CH34x driver is only needed if this particular unit routes the console
through a CH340/CH34x bridge (Linux: `ch341` is in-kernel; macOS/Windows: WCH
driver from wch.cn).

    # FULL erase + write (REQUIRED whenever the partition layout changed):
    esptool.py --chip esp32s3 --port <PORT> erase_flash
    esptool.py --chip esp32s3 --port <PORT> --baud 460800 write_flash 0x0 firmware-merged.bin

    # FAST app-only reflash (same partition layout):
    esptool.py --chip esp32s3 --port <PORT> --baud 460800 write_flash 0x10000 firmware.bin

If esptool can't sync: hold BOOT and tap RESET (or double-click RESET), re-run.

## First-boot operation

1. On a fresh flash (no WiFi stored) it starts a SoftAP **`MeshCore-Multi-Setup`**
   (password `meshcore`). Join it, open `http://192.168.4.1/`, enter your
   **2.4 GHz** network. It saves credentials and reboots into station mode.
2. Once on your LAN, browse to `https://<its-ip>/` (self-signed cert — accept
   the warning) and unlock with the admin password (default `password`).
3. A USB serial console at 115200 also works at all times and speaks the same
   command set. Type `help` for the list.

### Console command reference (serial or web panel)

    help                     list commands
    identities               list the three node pubkeys   <-- KEY liveness check
    wifi                     WiFi status (ssid/ip/rssi)
    set wifi.ssid <name>     set WiFi network (persists, own NVS store)
    set wifi.pwd <pass>      set WiFi password (persists)
    wifi reset               clear WiFi, reboot to setup AP
    <cmd>                    run <cmd> on the REPEATER (default target)
    repeater <cmd>           target the repeater   e.g. `repeater advert`
    room <cmd>               target the room       e.g. `room advert`
    companion <cmd>          STUB (companion HTTP surface is unbuilt - see below)
    # repeater/room accept stock CLI: advert | set lat -37.81 | set lon 144.96 | set name X | get ...

## Current status — READ THIS

- **VALIDATED ON HARDWARE (2026-07-25).** Flashed to the M5, boots to
  `=== boot complete ===`, `identities` returns three distinct pubkeys, and
  repeater/room adverts transmit cleanly (TX-done interrupt observed, no
  Dispatcher timeouts across repeated alternating sends).
- Two arbiter bugs were found and fixed during hardware bring-up:
  1. **The real radio's `begin()` was never called** — `Dispatcher::begin()`
     calls `_radio->begin()`, but `RadioPort` didn't override it (base is a
     no-op), so `RadioLibWrapper::begin()` never ran and the DIO1 ISR
     (RX-done/TX-done) was never attached: every TX timed out and RX was dead.
     Fix: `RadioPort::begin()` → `SharedRadioCore::beginRealOnce()`.
  2. **`pump()` clobbered in-flight transmits** — it called `recvRaw()` every
     cycle, which re-arms RX (`startReceive()`) whenever the driver isn't in
     STATE_RX, aborting an active TX. Fix: `pump()` returns early while
     `_tx_owner` is set; it also now services `_real->loop()` (noise-floor
     sampling), which previously never ran. `RadioPort::resetAGC()` is also
     gated while a TX is in flight (it warm-sleeps the radio).
- Known cosmetic issue: the 4th SPIFFS partition (`sys`) fails to mount —
  ESP-IDF's SPIFFS `max mounted partitions` limit is 3 in this Arduino core
  build. WiFi creds are unaffected (they live in NVS `multiwifi`), but
  composition prefs stored on `sys` are inert until this is addressed
  (raise CONFIG_SPIFFS_MAX_PARTITIONS or move `sys` prefs to NVS).
- Not yet verified over the air: adverts appearing on ANOTHER MeshCore node
  (none was available during bring-up), and RX/forwarding under real traffic.
- **First validation checkpoint:** flash it, get on WiFi (or use serial), run
  `identities`. If you get **three distinct pubkeys**, the composition is
  instantiating correctly. Then `repeater advert` / `room advert` and watch
  from another MeshCore node — repeater and room should appear as two separate
  nodes, repeater forwarding traffic.
- If it boot-loops or only one node appears or the radio is silent: the arbiter
  (`src/helpers/SharedRadio.{h,cpp}`) needs tuning under load. Capture the
  serial boot log (115200) — it prints each identity's pubkey at boot and any
  crash/backtrace.

## Companion app interface — DONE (2026-07-25)

The companion is now driveable by the **MeshCore phone app over WiFi/TCP port
5000**, using the stock `SerialWifiInterface` frame transport (the same thing a
stock `WIFI_SSID` companion build uses) instead of the previously planned HTTP
surface. `wrap_companion.cpp` swaps the old `NullSerialInterface` for
`SerialWifiInterface`; the listener starts lazily from `comp_loop()` once the
composition has WiFi up (station connected or setup AP active).
`SerialWifiInterface.cpp` was added to the multi env's `build_src_filter`.

Verified on hardware against a protocol-exact client: TCP connect →
`CMD_DEVICE_QUERY` → `RESP_CODE_DEVICE_INFO` ("Elecrow ThinkNode M5",
v1.16.0) → `CMD_APP_START` → `RESP_CODE_SELF_INFO` with the companion's exact
pubkey → `CMD_GET_CONTACTS` full sync. Repeater/room radio behaviour unaffected
during and after an app session. `companion status` on the console reports
listener + client state. In the app: add node via WiFi, the device's LAN IP,
port 5000.

## /multi web page — room management, companion messaging, debug (2026-07-25)

`https://<ip>/multi` (same login/session as the main panel; also linked from the
panel's Stats card). Tabs:
- **Debug** — identities, WiFi, uptime/heap, per-identity packet stats, repeater
  neighbours, and a live radio packet trace (dir/route/type/len/SNR/RSSI) from a
  ring buffer in `SharedRadioCore` (`pktLogCopy`). Each entry captures up to
  200 raw bytes (`PKT_RAW_CAP`); the page JS parses them to annotate source
  metadata: adverts → pubkey prefix + advertised name + lat/lon + distance from
  this node (SELF_INFO location); typed packets → src/dest hash with candidate
  contact/self names; flood path hop bytes.
- **Room** — advert, set name/location/password, ACL list, free-form room CLI
  console (goes through the existing `room <cmd>` dispatcher).
- **Messages / Channels** — full companion messaging from the browser. A
  `MuxSerialInterface` in `wrap_companion.cpp` lets the web inject app-protocol
  frames alongside the phone app's TCP session: responses (<0x80) route to
  whichever side issued the command, pushes (>=0x80) always go to the app.
  Verified on hardware: web and TCP sessions run concurrently with no
  cross-talk, and a DM sent from the web UI was ACKed over RF by a real node.
  Message viewing is NON-consuming: the mux mirrors every synced message frame
  (codes 7/8/16/17/27) into a 128-slot PSRAM archive
  (`GET /api/multi/comp/archive?after=N`), and the web Messages/Channels tabs
  render from that mirror. Messages appear in the web automatically whenever
  the phone app syncs them. The explicit "Pull from device" button (and
  auto-pull) still drains the device queue — needed when no phone app is in
  use — and its content flows to the UI via the same mirror (no duplicates).
  Archive is RAM-only; it resets on reboot (web keeps its own localStorage
  history and detects the seq reset).

New plumbing: `WebPanelServer::setExtRoutesRegistrar()` + `extAuthorize()`
(extension hook, upstream-friendly), `examples/multi_node/web_multi.cpp`
(page + endpoints), `examples/multi_node/multi_web.h` (glue),
`kWebReplyBufferSize` 256→1024. Endpoints: `GET /api/multi/debug`,
`GET /api/multi/packets?after=N`, `POST /api/multi/comp/frame` (binary).

## Unified web panel (2026-07-25, staged) — one UI at "/"

The composition page is now THE panel, served at `/` (index takeover via
`WebPanelServer::setExtOwnsIndex(true)`; stock SPA remains at `/app`, legacy
stats at `/stats`, old `/multi` 302-redirects to `/`). Tabs: Dashboard, Mesh,
Messages, Channels, Room, Repeater (full stock parity: name/clock-sync/
location/private-key/owner-info/admin+guest passwords/advert+flood intervals/
ghost mode), Radio (live-apply presets from api.meshcore.nz, manual params,
path hash mode, region), Stats (live tables + 24h sparklines from a
composition-native 1-min sampler in web_multi.cpp — the stock repeater stats
stack is compiled out in this build, so `/api/multi/stats?series=…` replaces
it), Debug (packet trace + general console with identity target dropdown),
Settings (manual OTA upload with in-browser MD5 — verified against RFC
vectors + firmware-sized buffer — via stock `/api/firmware-update`).
Sessions are shared with `/app` (`repeater-token` fallback; login no longer
rotates tokens). MQTT/bridge cards intentionally not ported (dead in this
build). Page lives in `examples/multi_node/web_multi_page.h`.

## Panel fixes (2026-07-25, delivered via OTA #2)

- **Radio presets**: api.meshcore.nz uses snake_case fields (`spreading_factor`
  etc.) — the picker now parses those, plus ships built-in presets (ANZ 915.8,
  AU Narrow 916.575, EU 869.525, US/CA 910.525 narrow — values from docs/
  cli_commands.md + faq.md) that work offline; community list appended+deduped.
- **Region select**: the dropdown is now the fixed AU state list (like stock —
  `region list allowed` only returns configured entries, initially just the
  wildcard `*`, so it can't populate a picker). Save runs region put/allowf/
  save; verified end-to-end (put→list shows `*,au,au-vic`→removed again).
- **Hashtag channels**: Channels tab has a Join box. Key = first 16 bytes of
  sha256("#name") per docs/companion_protocol.md (verified against the
  documented `#test` vector); CMD_SET_CHANNEL into the first free slot.
  `#test` is currently joined in slot 8 as a live demo.

## Panel polish (OTA #3): packet rows show wall-clock time (derived from device
## millis vs fetch time); hop paths render colon-separated with per-byte hover
## tooltips listing candidate nodes (contacts/self/neighbours first-byte match)
## in the packet trace, trace results, and mesh table; Debug console clears on
## Enter and Up/Down cycles the last 10 commands (persisted in localStorage).

## FLASHED AND VERIFIED (2026-07-25 late) — OTA layout live, USB now optional

The migration ran successfully: old SPIFFS partitions read out (flaky WCH
serial needed 115200 + retries for reads; writes fine), identity/contact files
unpacked and seeded into the new shared image, new partition table + firmware +
fs flashed with NVS untouched. Verified after reboot: **same three pubkeys**,
no FS mount warnings (sys bug gone), WiFi + radio params survived, contacts
intact (19), unified panel at `/`, `/multi` redirects, `/app` fallback works,
stats sampler running. Then the **first over-the-air update** was performed
through `/api/firmware-update` (1.6MB in 11.5s over WiFi) — device rebooted
into `ota_1` (confirmed via otadata) with identities intact. Future updates:
build, then upload firmware.bin via the panel's Settings tab (or POST with
X-Firmware-MD5). USB flashing is no longer required.

## Historical: staging notes from earlier in the session (all now applied)

The M5 was unplugged from USB mid-session (still up on WiFi). The following is
built clean and waiting for one esptool flash via `migrate_ota.sh` (in the
session scratchpad):

- **Session fixes**: /login now reuses the active token (second browser/device
  no longer kicks the first); idle auto-lock disabled
  (`WEB_PANEL_IDLE_TIMEOUT_MS=0`).
- **Mesh tab** on /multi: logical routing map (SVG: self → neighbours → contact
  out_path hop chains, flood-only contacts dashed) + known-nodes table +
  **Geo view** (Leaflet/OSM, plots nodes with advertised lat/lon) +
  **traceroute** (CMD_SEND_TRACE_PATH per contact; per-hop SNR drawn on the
  map; trace pushes (0x89) are mirrored into the archive, and the mux reports
  isConnected=true during recent web activity so trace results aren't dropped
  when no phone is attached).
- **OTA layout**: `partitions_multi_ota.csv` — dual 1.875MB app slots + ONE
  shared 192KB SPIFFS partition ("fs"), per-identity SubdirFS views
  (`/fs/rep|room|comp|sys`) over VFSImpl mountpoints. Fixes the sys-mount
  failure. Firmware at ~80% of a slot. The stock panel's
  `/api/firmware-update` (Update + auto-reboot) works once slots exist.
- **Migration** preserves everything: `migrate_ota.sh` reads the old SPIFFS
  partitions, unpacks with mkspiffs, seeds the new image via `pio buildfs`
  (`data/` tree), flashes partition table + app + fs WITHOUT touching NVS
  (WiFi/radio prefs survive). Companion private key + contacts also backed up
  over WiFi to the scratchpad as belt-and-braces.
- After migration verify: same three pubkeys, WiFi joins, no FS warnings; then
  prove OTA by POSTing firmware.bin to /api/firmware-update.

Known quirk: `SELF_INFO` reports the companion's OWN radio prefs
(869.618 MHz / 62.5 kHz / SF8 defaults), not the shared radio's actual params —
the composition's `multiradio` NVS store is authoritative for the real radio
(see "shared-radio ownership" in main.cpp). So the app *displays* wrong radio
settings, and radio-setting edits made from the app won't affect the real
radio. Cosmetic for normal chat use; fix later by intercepting/seeding the
companion's radio prefs from `g_radio` if it bothers anyone.

Optional nicety discussed but not built: per-persona TX power offset + jitter
(the arbiter already has a `TxPowerControl` hook wired via
`RealTxPower`/`setPortTxPower`) so the identities look like distinct nodes to
RSSI/timing correlation. Only matters if you want them to appear as separate
physical nodes; not needed for normal use.

## File map — what we added (everything else is stock EastMesh)

    src/helpers/SharedRadio.h / .cpp        the shared-radio arbiter (core of it)
    examples/multi_node/main.cpp            composition root: board/radio/WiFi/web/CLI/advert policy
    examples/multi_node/identity_module.h   the per-identity function-table interface
    examples/multi_node/wrap_repeater.cpp   renames+includes stock simple_repeater
    examples/multi_node/wrap_room.cpp       renames+includes stock simple_room_server
    examples/multi_node/wrap_companion.cpp  renames+includes stock companion_radio (+null serial)
    variants/thinknode_m5/partitions_multi.csv   app + rep/room/comp/sys SPIFFS partitions
    variants/thinknode_m5/platformio.ini    [env:ThinkNode_M5_multi] appended at end

## Key design constraints (don't "fix" these by accident)

- **Examples must stay unmodified.** All three roles are stock; the wrappers do
  the renaming via `#define MyMesh X` + `#include "../<ex>/MyMesh.cpp"`. If you
  need behaviour changes, do them in the composition or wrappers, not the
  examples — that's what lets us re-pull upstream for free.
- **Filesystem is SPIFFS**, one partition per identity (`rep`/`room`/`comp`) so
  each stock hardcoded `/identity` path is isolated, plus a `sys` partition for
  composition prefs. The stock examples are tested on SPIFFS; do not switch to
  LittleFS.
- **WiFi credentials live in a dedicated NVS namespace `multiwifi`** (see
  `wifiStoreGet/Set/Clear` in main.cpp), NOT in NetworkService's own store —
  because the repeater carries an inert stock `NetworkService` that runs first
  at boot and was clobbering the shared `eastmesh-net` NVS namespace. Keep WiFi
  creds in `multiwifi`.
- **Super-loop order matters:** `for each identity: loop(); then core->pump();`
  — all ports must consume the current radio frame before the next is fetched.
- **Single app slot, no OTA.** Three meshes + four SPIFFS partitions fill 4 MB;
  there isn't room for dual OTA app slots. Flashing is USB-only.
- Only **one** identity may forward floods (the repeater) or the box repeats
  every flood twice. Room/companion should not forward.
- 2.4 GHz WiFi only (ESP32-S3). Admin password default `password` — change via
  the repeater's stock `password <new>` if desired.

## Suggested first session for the local agent

1. `pio run -e ThinkNode_M5_multi -t mergebin`
2. Full-erase flash `firmware-merged.bin` at 0x0.
3. Serial monitor 115200 — confirm it boots to `=== boot complete ===` and the
   setup AP line, and that three `[repeater]/[room]/[companion] ID:` lines print.
4. Join `MeshCore-Multi-Setup`, set WiFi, reboot, confirm it re-joins and
   `identities` shows three pubkeys.
5. Report back: boot log, whether three IDs appear, whether adverts show on
   another node. That determines whether the arbiter is sound or needs work.
