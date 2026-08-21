#!/usr/bin/env python3
"""
Reliable BLE OTA flasher for nRF52 MeshCore nodes (Adafruit legacy DFU).

    uv run --with bleak python tools/meshflash.py <address-or-name> <firmware.zip>
    uv run --with bleak python tools/meshflash.py --recover <firmware.zip>

Why this exists
---------------
The previous script reported "activate+reset sent" and exited 0 having left the
node sitting in its bootloader, off the air, needing a physical double-tap to
recover. It transferred and CRC-validated the image perfectly and still bricked
the node for operational purposes, because it never checked whether the thing
came back.

Two defects produced that, and both are fixed here:

1. ACTIVATE was fire-and-forget. The old code did

       await c.write_gatt_char(CP, bytes([0x05]), response=False)

   inside a `try/except Exception: pass`, and then immediately left the
   `async with BleakClient(...)` block. A write-WITHOUT-response is unacknowledged
   and merely queued; the context manager's disconnect can tear the link down
   before it is ever transmitted. The bootloader never sees the activate, keeps
   its DFU flag set in GPREGRET, and every subsequent reset lands back in the
   bootloader -- which is precisely the observed behaviour, including a 0x06
   system reset returning to DFU rather than the app.

   Here ACTIVATE is written WITH response. That call is *expected* to raise a
   disconnection error, because the node resets while acknowledging it, so the
   disconnect is treated as the success signal rather than swallowed as failure.

2. There was no verification. Nothing confirmed the node came back as an
   application rather than as a bootloader. This tool does not exit 0 until it
   has seen the node advertising under its app name AND read its version back.

Everything else here follows from "never report success you have not observed".
"""

import argparse
import asyncio
import json
import struct
import sys
import time
import zipfile

from bleak import BleakClient, BleakScanner

DFU_SVC = "00001530-1212-efde-1523-785feabcd123"
DFU_CP = "00001531-1212-efde-1523-785feabcd123"
DFU_PKT = "00001532-1212-efde-1523-785feabcd123"

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Legacy Adafruit DFU opcodes.
OP_START = 0x01
OP_INIT = 0x02
OP_RECEIVE = 0x03
OP_VALIDATE = 0x04
OP_ACTIVATE = 0x05
OP_RESET = 0x06
OP_PRN = 0x08
RSP = 0x10
RECEIPT = 0x11

CHUNK = 20           # ATT payload at the default MTU
PRN = 32             # packet-receipt interval, in chunks

BOOTLOADER_NAMES = ("4631_DFU", "AdaDFU", "RAK_DFU")


def log(msg):
    print(msg, flush=True)


def fail(msg):
    print(f"FAIL: {msg}", flush=True)
    sys.exit(1)


# ---------------------------------------------------------------- discovery

async def find_by(pred, timeout, what):
    d = await BleakScanner.find_device_by_filter(pred, timeout=timeout)
    if d:
        log(f"  found {what}: {d.address} ({d.name!r})")
    return d


def is_bootloader(d, a):
    name = (a.local_name or d.name or "")
    if name in BOOTLOADER_NAMES:
        return True
    return DFU_SVC in [u.lower() for u in (a.service_uuids or [])]


async def find_bootloader(timeout=20):
    return await find_by(is_bootloader, timeout, "bootloader")


async def find_app(target, timeout=30):
    """Locate the application by address or by (sub)name."""
    t = target.lower()

    def pred(d, a):
        if d.address.lower() == t:
            return True
        name = (a.local_name or d.name or "")
        return bool(name) and t in name.lower()

    return await find_by(pred, timeout, "application")


# ---------------------------------------------------------------- app CLI

async def app_query(address, commands, timeout=25):
    """Run CLI commands over Nordic UART. Returns {cmd: reply}."""
    out = {}
    async with BleakClient(address, timeout=timeout) as c:
        q = asyncio.Queue()
        for _ in range(6):
            try:
                await c.start_notify(NUS_TX, lambda _h, d: q.put_nowait(bytes(d)))
                break
            except Exception:
                await asyncio.sleep(2)      # link still encrypting
        else:
            raise RuntimeError("NUS never became writable -- is the node paired?")

        for cmd in commands:
            while not q.empty():
                q.get_nowait()
            await c.write_gatt_char(NUS_RX, cmd.encode(), response=True)
            data = await asyncio.wait_for(q.get(), timeout=10)
            out[cmd] = data.decode(errors="replace").lstrip("> ").strip()
    return out


# ---------------------------------------------------------------- DFU

class Dfu:
    def __init__(self, client):
        self.c = client
        self.q = asyncio.Queue()

    async def arm(self):
        for i in range(8):
            try:
                await self.c.start_notify(DFU_CP, lambda _h, d: self.q.put_nowait(bytes(d)))
                return True
            except Exception as e:
                log(f"    control-point notify retry {i + 1}: {str(e)[:50]}")
                await asyncio.sleep(2)
        return False

    async def expect(self, op, timeout=60):
        while True:
            d = await asyncio.wait_for(self.q.get(), timeout=timeout)
            if d and d[0] == RSP and d[1] == op:
                if d[2] != 1:
                    raise RuntimeError(f"opcode {op:#04x} rejected, status={d[2]}"
                                       f"{' (INVALID_STATE)' if d[2] == 2 else ''}")
                return

    async def transfer(self, image, init):
        await self.c.write_gatt_char(DFU_CP, bytes([OP_START, 0x04]), response=True)
        await self.c.write_gatt_char(DFU_PKT, struct.pack("<III", 0, 0, len(image)), response=False)
        await self.expect(OP_START)
        log("  sizes accepted")

        await self.c.write_gatt_char(DFU_CP, bytes([OP_INIT, 0x00]), response=True)
        await self.c.write_gatt_char(DFU_PKT, init, response=False)
        await self.c.write_gatt_char(DFU_CP, bytes([OP_INIT, 0x01]), response=True)
        await self.expect(OP_INIT)
        log("  init packet accepted")

        await self.c.write_gatt_char(DFU_CP, struct.pack("<BH", OP_PRN, PRN), response=True)
        await self.c.write_gatt_char(DFU_CP, bytes([OP_RECEIVE]), response=True)

        t0, sent, since, last_pct = time.time(), 0, 0, -1
        for i in range(0, len(image), CHUNK):
            ch = image[i:i + CHUNK]
            await self.c.write_gatt_char(DFU_PKT, ch, response=False)
            sent += len(ch)
            since += 1
            if since >= PRN:
                since = 0
                d = await asyncio.wait_for(self.q.get(), timeout=30)
                if d[0] == RECEIPT:
                    got, = struct.unpack("<I", d[1:5])
                    # A mismatch means chunks were dropped. Continuing would
                    # produce an image that fails CRC 100+ seconds later, so
                    # stop at the point the evidence appears.
                    if got != sent:
                        raise RuntimeError(f"receipt mismatch: device has {got}, sent {sent}")
                pct = 100 * sent // len(image)
                if pct >= last_pct + 10:
                    last_pct = pct
                    log(f"    {pct:3d}%  {sent / (time.time() - t0) / 1024:.1f} kB/s")
        await self.expect(OP_RECEIVE, timeout=90)
        log(f"  image received ({time.time() - t0:.0f}s)")

        await self.c.write_gatt_char(DFU_CP, bytes([OP_VALIDATE]), response=True)
        await self.expect(OP_VALIDATE)
        log("  CRC validated")

    async def activate(self):
        """Swap in the image and reset.

        WITH response, deliberately. The node resets while acknowledging, so a
        disconnection here is the success signal -- whereas an unacknowledged
        write can be discarded at teardown before it is ever sent, which is the
        bug this whole tool exists to prevent. Any OTHER error is real.
        """
        try:
            await self.c.write_gatt_char(DFU_CP, bytes([OP_ACTIVATE]), response=True)
            log("  activate acknowledged")
        except Exception as e:
            txt = str(e).lower()
            if "disconn" in txt or "not connected" in txt:
                log("  activate sent (device reset while acknowledging -- expected)")
            else:
                raise


async def clear_stuck_session(dev):
    """Reset a bootloader that is mid-transfer so a fresh START is accepted.

    A bootloader that already completed a session answers START with
    status=2 (INVALID_STATE). A system reset returns it to a clean DFU state --
    it stays in the bootloader because the DFU flag is still set, which is
    exactly what we want here.
    """
    log("  bootloader busy, resetting its DFU state")
    try:
        async with BleakClient(dev, timeout=30) as c:
            d = Dfu(c)
            await d.arm()
            try:
                await c.write_gatt_char(DFU_CP, bytes([OP_RESET]), response=True)
            except Exception:
                pass
    except Exception:
        pass
    await asyncio.sleep(6)


# ---------------------------------------------------------------- verify

async def verify_app(target, expect_build=None, attempts=6):
    """Confirm the node is running the APPLICATION, not the bootloader.

    This is the check whose absence turned a good flash into a dead node. It is
    not optional and its failure is not a warning.
    """
    for i in range(attempts):
        await asyncio.sleep(5)
        boot = await BleakScanner.find_device_by_filter(is_bootloader, timeout=8)
        if boot:
            log(f"  [{i + 1}/{attempts}] still in bootloader")
            continue
        dev = await find_app(target, timeout=15)
        if not dev:
            log(f"  [{i + 1}/{attempts}] not advertising yet")
            continue
        try:
            info = await app_query(dev.address, ["ver", "get name", "get radio"])
        except Exception as e:
            log(f"  [{i + 1}/{attempts}] advertising but CLI not ready: {str(e)[:50]}")
            continue
        log(f"  ver   : {info.get('ver')}")
        log(f"  name  : {info.get('get name')}")
        log(f"  radio : {info.get('get radio')}")
        if expect_build and expect_build not in (info.get("ver") or ""):
            fail(f"running firmware is not the one we flashed "
                 f"(wanted build {expect_build!r}, got {info.get('ver')!r})")
        return info
    return None


# ---------------------------------------------------------------- main

async def run(args):
    z = zipfile.ZipFile(args.firmware)
    man = json.loads(z.read("manifest.json"))["manifest"]["application"]
    image = z.read(man["bin_file"])
    init = z.read(man["dat_file"])
    crc = man["init_packet_data"]["firmware_crc16"]
    log(f"image {len(image)}B  init {len(init)}B  crc16={crc}")

    before = None
    if not args.recover:
        # Record what must survive. If identity changes across a flash we want
        # to say so loudly rather than discover it days later.
        log("pre-flight: reading current state")
        dev = await find_app(args.target, timeout=25)
        if not dev:
            fail(f"{args.target!r} not found. If it is already in the bootloader, "
                 f"re-run with --recover")
        try:
            before = await app_query(dev.address, ["ver", "get name", "get radio"])
            for k, v in before.items():
                log(f"  {k:12s} {v}")
        except Exception as e:
            log(f"  (could not read CLI: {str(e)[:60]})")

        log("stage 1: triggering bootloader")
        try:
            async with BleakClient(dev.address, timeout=40) as c:
                d = Dfu(c)
                await d.arm()
                try:
                    await c.write_gatt_char(DFU_CP, b"\x01", response=True)
                except Exception:
                    pass                    # resets while acknowledging
        except Exception:
            pass
        await asyncio.sleep(2.5)

    log("stage 2: locating bootloader")
    boot = await find_bootloader(timeout=20)
    if not boot:
        fail("bootloader not found -- node may not have entered DFU")

    for attempt in range(2):
        try:
            async with BleakClient(boot, timeout=30) as c:
                d = Dfu(c)
                if not await d.arm():
                    fail("could not arm the DFU control point")
                await d.transfer(image, init)
                await d.activate()
            break
        except RuntimeError as e:
            if "INVALID_STATE" in str(e) and attempt == 0:
                await clear_stuck_session(boot)
                boot = await find_bootloader(timeout=20) or boot
                continue
            fail(str(e))
        except Exception as e:
            fail(f"transfer failed: {str(e)[:120]}")

    log("stage 3: verifying the node came back")
    target = args.target
    if args.recover and before is None:
        target = args.expect_name or "MeshCore"
    info = await verify_app(target, expect_build=args.expect_build)

    if not info:
        print("", flush=True)
        fail("node did not return to the application.\n"
             "  It is almost certainly sitting in its bootloader.\n"
             "  Recover it by double-tapping RESET (mounts a UF2 volume) and copying\n"
             "  a .uf2 built from the same firmware, then investigate before retrying.")

    if before:
        for key in ("get name", "get radio"):
            if before.get(key) and before[key] != info.get(key):
                log(f"WARNING: {key} changed across the flash: "
                    f"{before[key]!r} -> {info.get(key)!r}")
    log("OK: flashed and verified running")
    return 0


def main():
    p = argparse.ArgumentParser(description="Reliable BLE OTA flasher for MeshCore nRF52 nodes")
    p.add_argument("target", nargs="?", default="",
                   help="BLE address or a substring of the node name")
    p.add_argument("firmware", help="path to firmware.zip (PlatformIO DFU package)")
    p.add_argument("--recover", action="store_true",
                   help="skip the trigger step; the node is already in its bootloader")
    p.add_argument("--expect-build", default=None,
                   help="require this string in 'ver' after flashing, e.g. '17 Aug 2026'")
    p.add_argument("--expect-name", default=None,
                   help="app name substring to look for when using --recover")
    args = p.parse_args()
    if not args.recover and not args.target:
        p.error("target is required unless --recover is given")
    sys.exit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
