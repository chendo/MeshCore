#!/usr/bin/env python3
"""
meshweb -- a management dashboard for MeshCore nRF52 repeaters.

These boards are nRF52840: no WiFi, so unlike the ESP32 targets they cannot host
a panel themselves. This runs on a host instead and reaches each node over the
transports they do have -- USB serial, or the BLE diagnostic port -- polls the
CLI, and serves the result as a single page.

    uv run --with aiohttp --with pyserial --with bleak python meshweb.py \
        --serial VIC-Dev=/dev/cu.usbmodem1101 \
        --ble    VIC-Prod=0C4FDA90-889B-26F9-BEAE-4BA9E27F3B43

Everything is read through the same CLI a human would type, so the dashboard can
never show something you could not confirm by hand.
"""

import argparse, asyncio, json, re, sys, time
from pathlib import Path

from aiohttp import web

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # host -> node
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # node -> host

# The observer table can hold MAX_PEERS; the CLI hands them over one at a time
# because a reply is capped at 160 bytes.
MAX_PEER_FETCH = 48


# ---------------------------------------------------------------- transports

class SerialTransport:
    """USB CDC. Blocking pyserial, kept off the event loop with to_thread."""

    kind = "serial"

    def __init__(self, port):
        self.port = port
        self._ser = None
        self._lock = asyncio.Lock()

    def _open(self):
        import serial
        if self._ser is None or not self._ser.is_open:
            self._ser = serial.Serial(self.port, 115200, timeout=0.4)
            time.sleep(0.4)
            self._ser.reset_input_buffer()
        return self._ser

    def _cmd(self, command):
        s = self._open()
        s.reset_input_buffer()
        s.write((command + "\r\n").encode())
        s.flush()
        deadline, buf = time.time() + 2.0, b""
        while time.time() < deadline:
            chunk = s.read(2000)
            if chunk:
                buf += chunk
                if b"\r\n" in buf[buf.find(b"->") :] and b"->" in buf:
                    break
            elif buf:
                break
        return _last_reply(buf.decode(errors="replace"))

    async def send(self, command):
        async with self._lock:
            try:
                return await asyncio.to_thread(self._cmd, command)
            except Exception as e:
                self._ser = None
                raise RuntimeError(f"serial: {e}") from e


class BleTransport:
    """
    The Nordic UART diagnostic port. The link must be encrypted before the
    characteristics will talk, so a first run needs the node's per-boot PIN
    entered at the OS pairing prompt; after that the bond is reused.
    """

    kind = "ble"

    def __init__(self, address):
        self.address = address
        self._client = None
        self._q = None
        self._lock = asyncio.Lock()

    async def _connect(self):
        from bleak import BleakClient
        if self._client is not None and self._client.is_connected:
            return self._client
        self._q = asyncio.Queue()
        c = BleakClient(self.address, timeout=30)
        await c.connect()
        for _ in range(6):
            try:
                await c.start_notify(NUS_TX, lambda _h, d: self._q.put_nowait(bytes(d)))
                break
            except Exception:
                await asyncio.sleep(2)      # waiting for the link to encrypt
        else:
            await c.disconnect()
            raise RuntimeError("link never encrypted -- pair the node first")
        self._client = c
        return c

    async def send(self, command):
        async with self._lock:
            try:
                c = await self._connect()
                while not self._q.empty():
                    self._q.get_nowait()
                await c.write_gatt_char(NUS_RX, command.encode(), response=True)
                data = await asyncio.wait_for(self._q.get(), timeout=8)
                return _strip_prefix(data.decode(errors="replace"))
            except Exception as e:
                self._client = None
                raise RuntimeError(f"ble: {e}") from e


def _strip_prefix(s):
    """
    `get` answers are prefixed "> " by the CLI. Serial replies come wrapped in
    the echoed command so _last_reply peels them; a BLE reply is the bare frame
    and keeps its prefix, which silently broke every numeric parse.
    """
    s = s.strip()
    while s.startswith("->") or s.startswith(">"):
        s = s.lstrip("->").lstrip(">").strip()
    return s


def _last_reply(text):
    """The CLI echoes the command, then answers with '  -> ...'."""
    out = ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("->"):
            out = line[2:].strip()
        elif line.startswith("> ") and not out:
            out = line[2:].strip()
    return out.lstrip("> ").strip()


# ------------------------------------------------------------------ parsing

def _json_or_none(s):
    try:
        return json.loads(s)
    except Exception:
        return None


def parse_hops(s):
    # "hops seen (of 427 frames): 0:10 1:22 2:47 ..."
    m = re.search(r"of (\d+) frames\):(.*)", s)
    if not m:
        return None
    bins = {int(k): int(v) for k, v in re.findall(r"(\d+):(\d+)", m.group(2))}
    return {"frames": int(m.group(1)), "bins": bins}


def parse_types(s):
    # "types: REQ:71 RESP:37 TXT:14 ..."
    if not s.startswith("types:"):
        return None
    return {k: int(v) for k, v in re.findall(r"([A-Z]+):(\d+)", s)}


def parse_heard(s):
    # "floods sent 143, confirmed relayed 37 (25%); by hash width 2B:49 1B:75(ignored)..."
    m = re.search(r"floods sent (\d+), confirmed relayed (\d+) \((\d+)%\)", s)
    if not m:
        return None
    return {"sent": int(m.group(1)), "confirmed": int(m.group(2)), "pct": int(m.group(3))}


def parse_peers_summary(s):
    # "11/48 peers (11x1B collision-prone), 0 confirmed hearing us; evicted 0, refused 0: ..."
    m = re.search(r"(\d+)/(\d+) peers \((\d+)x1B[^)]*\), (\d+) confirmed[^;]*; evicted (\d+), refused (\d+)", s)
    if not m:
        return None
    g = [int(x) for x in m.groups()]
    return {"count": g[0], "max": g[1], "width1": g[2],
            "confirmed": g[3], "evicted": g[4], "refused": g[5]}


def parse_bridge(s):
    # "ble bridge up: tx 3 drop 0 | rx seen 1855 ok 89 dup 44 bad 0 other 1722 | peers 1"
    m = re.search(r"ble bridge (\w+): tx (\d+) drop (\d+) \| rx seen (\d+) ok (\d+) "
                  r"dup (\d+) bad (\d+) other (\d+) \| peers (\d+)", s)
    if not m:
        return None
    return {"state": m.group(1), "tx": int(m.group(2)), "drop": int(m.group(3)),
            "seen": int(m.group(4)), "ok": int(m.group(5)), "dup": int(m.group(6)),
            "bad": int(m.group(7)), "other": int(m.group(8)), "peers": int(m.group(9))}


def parse_radio_cfg(s):
    p = s.split(",")
    if len(p) != 4:
        return None
    return {"freq": float(p[0]), "bw": float(p[1]), "sf": int(p[2]), "cr": int(p[3])}


# -------------------------------------------------------------------- node

class Node:
    def __init__(self, name, transport):
        self.name = name
        self.tx = transport
        self.state = {"name": name, "transport": transport.kind, "online": False,
                      "error": None, "updated": 0}

    async def ask(self, cmd):
        return _strip_prefix(await self.tx.send(cmd))

    async def poll(self, with_peers):
        """
        Two rates. The fast metrics are a dozen round-trips; the peer table is
        one round-trip PER PEER and can be 48 of them, which over BLE is slower
        than the poll interval. Fetching it every cycle would make a second node
        wait behind the first, so it runs on a slower cadence and the previous
        table is kept in between.
        """
        st = {"name": self.name, "transport": self.tx.kind, "error": None}
        try:
            st["ver"] = await self.ask("ver")
            st["node_name"] = await self.ask("get name")
            st["radio"] = parse_radio_cfg(await self.ask("get radio"))
            st["tx_power"] = await self.ask("get tx")
            st["stats_radio"] = _json_or_none(await self.ask("stats-radio"))
            st["stats_packets"] = _json_or_none(await self.ask("stats-packets"))
            st["stats_core"] = _json_or_none(await self.ask("stats-core"))
            st["hops"] = parse_hops(await self.ask("hops"))
            st["types"] = parse_types(await self.ask("types"))
            st["heard"] = parse_heard(await self.ask("heard"))
            st["bridge"] = parse_bridge(await self.ask("bridge"))
            summary = await self.ask("peers")
            st["peers_summary"] = parse_peers_summary(summary)

            if with_peers:
                peers, total = [], (st["peers_summary"] or {}).get("count", 0)
                for i in range(min(total, MAX_PEER_FETCH)):
                    entry = _json_or_none(await self.ask(f"peers {i}"))
                    if not entry or "h" not in entry:
                        continue
                    peers.append(entry)
                st["peers"] = peers
                st["peers_updated"] = time.time()
            else:
                st["peers"] = self.state.get("peers", [])
                st["peers_updated"] = self.state.get("peers_updated", 0)
            st["online"] = True
        except Exception as e:
            st["online"] = False
            st["error"] = str(e)[:160]
            # keep the last good peer table so one dropped poll does not blank
            # the view; the age is shown so it is obvious it is stale
            st["peers"] = self.state.get("peers", [])
            st["peers_updated"] = self.state.get("peers_updated", 0)
        st["updated"] = time.time()
        self.state = st


# ------------------------------------------------------------------ server

async def poller(nodes, interval, peers_every):
    """
    Nodes are polled CONCURRENTLY. Sequentially, one unreachable node would hold
    every other node's data hostage for its whole timeout -- exactly when you
    most want to see the others.
    """
    cycle = 0
    while True:
        with_peers = (cycle % max(1, peers_every) == 0)
        await asyncio.gather(*(n.poll(with_peers) for n in nodes),
                             return_exceptions=True)
        cycle += 1
        await asyncio.sleep(interval)


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", action="append", default=[], metavar="NAME=PORT")
    ap.add_argument("--ble", action="append", default=[], metavar="NAME=ADDRESS")
    ap.add_argument("--port", type=int, default=8712)
    ap.add_argument("--interval", type=float, default=15.0,
                    help="seconds between polls (each poll is many CLI round-trips)")
    ap.add_argument("--peers-every", type=int, default=3,
                    help="fetch the full peer table every Nth poll (it is one "
                         "round-trip per peer, so it is the expensive part)")
    args = ap.parse_args()

    nodes = []
    for spec in args.serial:
        name, _, port = spec.partition("=")
        nodes.append(Node(name or port, SerialTransport(port)))
    for spec in args.ble:
        name, _, addr = spec.partition("=")
        nodes.append(Node(name or addr, BleTransport(addr)))
    if not nodes:
        ap.error("give at least one --serial or --ble node")

    here = Path(__file__).parent
    app = web.Application()

    async def index(_req):
        return web.FileResponse(here / "index.html")

    async def state(_req):
        return web.json_response({"nodes": [n.state for n in nodes],
                                  "now": time.time()})

    async def command(req):
        body = await req.json()
        target, cmd = body.get("node"), body["cmd"]
        targets = nodes if target in (None, "", "*") else [n for n in nodes if n.name == target]
        if not targets:
            return web.json_response({"replies": {"": "no such node"}}, status=404)

        async def one(n):
            try:
                return n.name, await n.ask(cmd)
            except Exception as e:
                return n.name, f"error: {e}"
        pairs = await asyncio.gather(*(one(n) for n in targets))
        return web.json_response({"replies": dict(pairs)})

    app.router.add_get("/", index)
    app.router.add_get("/api/state", state)
    app.router.add_post("/api/cmd", command)

    asyncio.create_task(poller(nodes, args.interval, args.peers_every))
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, "127.0.0.1", args.port).start()
    print(f"meshweb on http://127.0.0.1:{args.port}  "
          f"({len(nodes)} node(s), polling every {args.interval:g}s)", flush=True)
    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
