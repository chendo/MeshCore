#!/usr/bin/env python3
"""Log a MeshCore node's battery and power state over BLE, on an interval.

The RAK3401 has no current sensing, so charge and discharge are MODELLED: the
firmware differentiates a filtered voltage series against the cell capacity
declared in the build. One reading therefore says very little — the rate only
becomes meaningful once the series is long enough for the model to have a trend,
which is the whole reason this logs rather than polls once.

Three numbers here come from different places and will not agree exactly:
  power.mV      the battery model's own filtered sample     (`power`)
  core.mV       a direct 8-sample ADC read                  (`stats-core`)
  boot.mV       the sample latched at boot, under no load   (`get pwrmgt.bootmv`)
Roughly 70mV apart is normal. Chart one or the other, never both as one series.
The mv/pct columns say which of the three they came from, so filter on mv_src
before charting.

WHAT ACTUALLY ANSWERS OVER BLE (checked against v1.17.1, Build 14 Aug 2026, on
both nodes): only `get pwrmgt.*`. Both `power` and `stats-core` come back
"Unknown command" on this build -- stats-core is serial-gated in firmware, and
`power` does not exist here at all despite older notes describing it. They are
still asked for, cheaply, so a newer build starts filling those columns without
a code change; until then mv comes from bootmv, and the uptime and reboot
columns stay empty because uptime only ever came from stats-core.

Because bootmv is LATCHED AT BOOT it does not move between polls. A flat mV
series is therefore the expected reading on this firmware, not a stuck sensor.

`pwrmgt.source` is the only HARDWARE fact of the four: it reads the nRF52's USB
regulator VBUS-detect bit, so "external" means real 5V is present rather than
inferred from a voltage curve. It says nothing about whether the pack is
charging, or even present -- so voltage and percentage are reported alongside
it, never suppressed by it.

    uv run --with bleak python tools/battery_watch.py            # both nodes
    uv run --with bleak python tools/battery_watch.py --once     # single pass
    uv run --with bleak python tools/battery_watch.py --interval 300

A node that is switched off is expected, not an error: it is logged offline and
polling continues.
"""
import argparse
import asyncio
import csv
import os
import re
import sys
import time
from datetime import datetime, timezone

from bleak import BleakClient

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # host -> node
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # node -> host

NODES = {
    "prod": "0C4FDA90-889B-26F9-BEAE-4BA9E27F3B43",   # VIC-NorthcoteNW-DIS
    "dev":  "7E813F21-9D70-98BE-E388-19F014960580",   # VIC-Northcote-BRIDGEMid
}

CSV_FIELDS = ["utc", "node", "online", "source", "mv", "mv_src", "pct",
              "power_mv", "core_mv", "boot_mv",
              "ma", "mw", "mv_per_hr", "eta", "quality", "samples",
              "uptime_s", "rebooted", "error"]

# power: 4015mV 79% +0mA +0mW (+0mV/hr); eta unknown; q2 n120
POWER_RE = re.compile(
    r"power:\s*(\d+)mV\s+(\d+)%\s*([+-]?\d+)mA\s*([+-]?\d+)mW\s*\(([+-]?\d+)mV/hr\);"
    r"\s*(.*?);\s*q(\d+)\s+n(\d+)")
CORE_RE = re.compile(r'"battery_mv":(\d+).*?"uptime_secs":(\d+)')
# "> 3979 mV", sometimes with an unrelated async line glued to the tail
BOOTMV_RE = re.compile(r"(\d+)\s*mV")

# Single-cell LiPo resting voltage -> state of charge. APPROXIMATE, and worth
# two caveats before anyone reads a number off it as truth:
#   * a cell's terminal voltage depends on load and temperature as well as
#     charge, and boot.mV in particular is sampled under near-zero load;
#   * a reading taken while the node is on EXTERNAL power sits ABOVE the true
#     resting cell voltage, because the charger holds the pack up. Expect an
#     optimistic percentage there -- often ~100% on a pack that is only part
#     charged. Trust these figures on "battery", treat them as a ceiling on
#     "external".
# 4.20V = full, ~3.70V nominal, 3.00-3.20V = empty.
LIPO_CURVE = [(4200, 100), (4100, 90), (4000, 80), (3930, 70), (3870, 60),
              (3840, 50), (3800, 40), (3770, 30), (3730, 20), (3690, 10),
              (3610, 5), (3200, 0), (3000, 0)]


def lipo_pct(mv):
    """Percentage for a single-cell LiPo at `mv` millivolts, by linear
    interpolation across LIPO_CURVE. Clamped at both ends."""
    if not mv:
        return None
    if mv >= LIPO_CURVE[0][0]:
        return 100
    for (hi_mv, hi_p), (lo_mv, lo_p) in zip(LIPO_CURVE, LIPO_CURVE[1:]):
        if mv >= lo_mv:
            return int(round(lo_p + (hi_p - lo_p) * (mv - lo_mv) / (hi_mv - lo_mv)))
    return 0


def strip(s):
    s = s.strip()
    for p in ("->", ">"):
        while s.startswith(p):
            s = s[len(p):].strip()
    return s


async def ask(client, q, cmd, timeout=8.0):
    while not q.empty():
        q.get_nowait()
    await client.write_gatt_char(NUS_RX, cmd.encode(), response=True)
    data = await asyncio.wait_for(q.get(), timeout=timeout)
    # a long reply arrives over several notifications
    while True:
        try:
            data += await asyncio.wait_for(q.get(), timeout=0.6)
        except asyncio.TimeoutError:
            break
    return strip(data.decode(errors="replace"))


async def poll(name, address):
    """One node, one pass. Never raises: an off node is a normal outcome."""
    row = {k: "" for k in CSV_FIELDS}
    row["utc"] = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    row["node"] = name
    row["online"] = "no"
    q = asyncio.Queue()
    try:
        async with BleakClient(address, timeout=25) as c:
            # the link has to encrypt before the characteristics will talk
            for _ in range(10):
                try:
                    await c.start_notify(NUS_TX, lambda _h, d: q.put_nowait(bytes(d)))
                    break
                except Exception:
                    await asyncio.sleep(2)
            else:
                row["error"] = "link never encrypted (needs pairing?)"
                return row

            row["online"] = "yes"
            src = await ask(c, q, "get pwrmgt.source")
            # an unrelated async line can be glued to the reply; keep the verdict
            row["source"] = ("external" if "external" in src else
                             "battery" if "battery" in src else src[:20])

            # `power` and `stats-core` are absent on v1.17.1 over BLE (see the
            # module docstring). Asked anyway so a newer build fills them in.
            m = POWER_RE.search(await ask(c, q, "power"))
            if m:
                (row["power_mv"], row["pct"], row["ma"], row["mw"], row["mv_per_hr"],
                 row["eta"], row["quality"], row["samples"]) = m.groups()

            m = CORE_RE.search(await ask(c, q, "stats-core"))
            if m:
                row["core_mv"], row["uptime_s"] = m.group(1), m.group(2)

            m = BOOTMV_RE.search(await ask(c, q, "get pwrmgt.bootmv"))
            if m:
                row["boot_mv"] = m.group(1)

            # Voltage and percentage are reported whatever pwrmgt.source says:
            # best live reading first, boot sample as the floor of last resort.
            for field, label in (("power_mv", "model"), ("core_mv", "adc"),
                                 ("boot_mv", "boot")):
                if row[field]:
                    row["mv"], row["mv_src"] = row[field], label
                    break
            if row["mv"] and not row["pct"]:
                row["pct"] = str(lipo_pct(int(row["mv"])))
    except Exception as e:
        row["error"] = str(e)[:120]
    return row


def fmt(row, prev_uptime):
    if row["online"] != "yes":
        return f"  {row['node']:<5} offline    {row['error']}"
    reb = ""
    if prev_uptime is not None and row["uptime_s"]:
        if int(row["uptime_s"]) < prev_uptime:
            reb = "  ** REBOOTED **"
            row["rebooted"] = "yes"
    up_h = int(row["uptime_s"]) / 3600.0 if row["uptime_s"] else 0
    mv = f"{row['mv']}mV" if row["mv"] else "?mV"
    pct = f"{row['pct']}%" if row["pct"] else "?%"
    # voltage and pct lead, and the power source rides alongside them
    return (f"  {row['node']:<5} {mv:>7} {pct:>5} ({row['mv_src'] or '-':<5}) "
            f"on {row['source']:<8} "
            f"model {row['power_mv'] or '-':>4}mV  adc {row['core_mv'] or '-':>4}mV  "
            f"{row['ma'] or '-':>4}mA {row['mv_per_hr'] or '-':>4}mV/hr  "
            f"eta {row['eta'] or '-':<10} up {up_h:5.1f}h  "
            f"q{row['quality'] or '-'} n{row['samples'] or '-'}{reb}")


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--interval", type=float, default=900.0, help="seconds (default 900 = 15 min)")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--csv", default=os.path.expanduser("~/meshcore-battery.csv"))
    ap.add_argument("--node", action="append", help="name=ADDRESS (repeatable)")
    args = ap.parse_args()

    nodes = dict(NODES)
    if args.node:
        nodes = {}
        for spec in args.node:
            n, _, a = spec.partition("=")
            nodes[n] = a

    fresh = not os.path.exists(args.csv)
    if not fresh:
        # appending new columns under an old header would silently misalign
        # every later row, so retire the old file rather than corrupt it
        with open(args.csv, newline="") as old:
            header = next(csv.reader(old), [])
        if header != CSV_FIELDS:
            retired = args.csv + ".v1"
            os.rename(args.csv, retired)
            print(f"csv columns changed; previous log kept at {retired}")
            fresh = True
    fh = open(args.csv, "a", newline="")
    w = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
    if fresh:
        w.writeheader()
        fh.flush()

    print(f"battery_watch: {len(nodes)} node(s), every {args.interval/60:.0f} min")
    print(f"logging to {args.csv}   (ctrl-C to stop)\n")

    last_uptime = {n: None for n in nodes}
    while True:
        print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        for name, addr in nodes.items():
            row = await poll(name, addr)
            print(fmt(row, last_uptime[name]), flush=True)
            if row["uptime_s"]:
                last_uptime[name] = int(row["uptime_s"])
            w.writerow(row)
            fh.flush()
        if args.once:
            break
        print()
        await asyncio.sleep(args.interval)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped")
