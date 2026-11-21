#!/usr/bin/env python3
"""Run MeshCore CLI commands on a node over its Nordic UART diagnostic port.

For nodes reachable by BLE but not by USB — which is both RAK repeaters here.
The link must already be bonded; a first connection prompts for the node's PIN
at the OS pairing dialog.

    tools/ble_cli.py prod ver "get radio" stats-core
    tools/ble_cli.py 7E813F21-... "clock"

Known nodes can be named instead of pasting a UUID. A long reply arrives over
several notifications, so each command drains until the stream goes quiet rather
than taking the first packet as the whole answer.
"""
import asyncio
import sys

from bleak import BleakClient

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # host -> node
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # node -> host

NODES = {
    "prod": "0C4FDA90-889B-26F9-BEAE-4BA9E27F3B43",   # VIC-NorthcoteNW-DIS
    "dev":  "7E813F21-9D70-98BE-E388-19F014960580",   # VIC-Northcote-BRIDGEMid
}


def strip(s):
    s = s.strip()
    for p in ("->", ">"):
        while s.startswith(p):
            s = s[len(p):].strip()
    return s


async def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    target = sys.argv[1]
    address = NODES.get(target, target)
    cmds = sys.argv[2:]

    q = asyncio.Queue()
    async with BleakClient(address, timeout=30) as c:
        for _ in range(10):
            try:
                await c.start_notify(NUS_TX, lambda _h, d: q.put_nowait(bytes(d)))
                break
            except Exception:
                await asyncio.sleep(2)      # link still encrypting
        else:
            print("link never encrypted — pair the node first")
            return
        for cmd in cmds:
            while not q.empty():
                q.get_nowait()
            try:
                await c.write_gatt_char(NUS_RX, cmd.encode(), response=True)
                data = await asyncio.wait_for(q.get(), timeout=10)
                while True:
                    try:
                        data += await asyncio.wait_for(q.get(), timeout=0.6)
                    except asyncio.TimeoutError:
                        break
                print(f"  {cmd:24s} {strip(data.decode(errors='replace'))}")
            except asyncio.TimeoutError:
                print(f"  {cmd:24s} <no reply>")
            except Exception as e:
                print(f"  {cmd:24s} <error: {str(e)[:70]}>")


asyncio.run(main())
