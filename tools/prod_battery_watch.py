#!/usr/bin/env python3
"""Log prod's battery over the MESH, and summarise how it trended in a day.

Prod has no USB and is not always in BLE range, so this goes the long way: the
M5's companion identity logs in over LoRa and issues CMD_SEND_STATUS_REQ — the
same request the phone app makes. That matters, because the status reply carries
`getBatteryMilliVolts(true)`, a LIVE reading. The CLI on prod's current build
offers only `pwrmgt.bootmv`, which is latched at boot and never moves, so a log
built on it records the same number forever.

    # collect, forever, every 15 minutes
    MESHCORE_PROD_PASSWORD='...' tools/prod_battery_watch.py

    # what happened today (what the 9pm job runs)
    tools/prod_battery_watch.py --report

The admin password is read from MESHCORE_PROD_PASSWORD, never stored here — this
file is committed to a public repo.

A poll can fail simply because a flood was lost; that is normal on a mesh and is
recorded as a miss rather than treated as an outage. Only a run of consecutive
misses means anything, so the report counts them.
"""
import argparse
import csv
import importlib.util
import os
import statistics
import struct
import sys
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
PROD_KEY = "3070D00B1E0683E60B42A32B89682202E2E515600BC35BF57DE9608A1CA1907F".lower()
DEFAULT_CSV = os.path.expanduser("~/prod-battery.csv")

FIELDS = ["utc", "ok", "battery_mv", "uptime_s", "noise_floor", "last_rssi",
          "last_snr", "n_recv", "n_sent", "n_recv_errors", "tx_air_s", "rx_air_s",
          "tx_queue", "err_events", "flood_dups", "error"]

# see examples/simple_repeater/MyMesh.h struct RepeaterStats — 56 bytes, LE
STATS_FMT = "<HHhhIIIIIIIIHhHHII"


def load_mesh_admin():
    spec = importlib.util.spec_from_file_location("mesh_admin",
                                                  os.path.join(HERE, "mesh_admin.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def poll_once(ma, host, panel_pw, key, admin_pw, wait):
    """One status request. Returns a row dict; never raises."""
    row = {k: "" for k in FIELDS}
    row["utc"] = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    row["ok"] = "no"
    try:
        tok = ma.login_panel(host, panel_pw)
        ma.app_start(host, tok)
        latest, _ = ma.archive_after(host, tok, 0)
        ma.send_frame(host, tok, bytearray([26]) + bytes.fromhex(key) + admin_pw.encode())
        if not ma.wait_for(host, tok, latest, {0x85, 0x86}, 20):
            row["error"] = "login: no answer"
            return row
        latest, _ = ma.archive_after(host, tok, latest)
        ma.send_frame(host, tok, bytearray([27]) + bytes.fromhex(key))
        hit = ma.wait_for(host, tok, latest, {0x87}, wait)
        if not hit:
            row["error"] = "status: no answer"
            return row
        v = struct.unpack_from(STATS_FMT, hit[1], 8)
        (batt, txq, noise, rssi, precv, psent, air, up, _sf, _sd,
         _rf, _rd, errs, snr4, _dd, fdups, rxair, rerrs) = v
        row.update(ok="yes", battery_mv=batt, uptime_s=up, noise_floor=noise,
                   last_rssi=rssi, last_snr=f"{snr4/4:.2f}", n_recv=precv,
                   n_sent=psent, n_recv_errors=rerrs, tx_air_s=air, rx_air_s=rxair,
                   tx_queue=txq, err_events=errs, flood_dups=fdups)
    except Exception as e:
        row["error"] = str(e)[:120]
    return row


def report(path, day=None):
    if not os.path.exists(path):
        print(f"no log at {path}")
        return 1
    rows = list(csv.DictReader(open(path)))
    # Rows are stamped UTC, but "how did it trend through the day" means the
    # LOCAL day the user lives in — at 9pm local those differ, so filtering on
    # the raw UTC string silently reports an empty day.
    day = day or datetime.now().astimezone().strftime("%Y-%m-%d")
    for r in rows:
        r["_local"] = (datetime.strptime(r["utc"], "%Y-%m-%d %H:%M:%S")
                       .replace(tzinfo=timezone.utc).astimezone())
    todays = [r for r in rows if r["_local"].strftime("%Y-%m-%d") == day]
    if not todays:
        print(f"prod battery — {day}\n  no samples logged")
        return 0
    ok = [r for r in todays if r["ok"] == "yes"]
    mv = [int(r["battery_mv"]) for r in ok if r["battery_mv"]]

    print(f"prod battery — {day} (local)")
    print(f"  polls {len(todays)}, answered {len(ok)}, missed {len(todays)-len(ok)}")
    if not mv:
        print("  no successful readings — prod did not answer all day")
        return 0
    print(f"  window   {ok[0]['_local'].strftime('%H:%M')} to {ok[-1]['_local'].strftime('%H:%M')}")

    first, last = mv[0], mv[-1]
    span_h = 0.0
    if len(ok) > 1:
        span_h = (ok[-1]["_local"] - ok[0]["_local"]).total_seconds() / 3600.0
    rate = (last - first) / span_h if span_h else 0.0
    arrow = "falling" if last < first else ("rising" if last > first else "flat")
    print(f"  battery  {first} -> {last} mV   ({last-first:+d} mV over {span_h:.1f} h, "
          f"{rate:+.1f} mV/h, {arrow})")
    print(f"  range    min {min(mv)}  max {max(mv)}  median {int(statistics.median(mv))} mV")

    # a reboot resets uptime; worth surfacing because it also resets the counters
    ups = [int(r["uptime_s"]) for r in ok if r["uptime_s"]]
    reboots = sum(1 for a, b in zip(ups, ups[1:]) if b < a)
    if ups:
        print(f"  uptime   {ups[-1]/86400:.2f} d" +
              (f"   ** {reboots} reboot(s) detected today **" if reboots else ""))

    # receive-error rate is the other number that has been drifting
    lastok = ok[-1]
    if lastok["n_recv"] and lastok["n_recv_errors"]:
        rc, re_ = int(lastok["n_recv"]), int(lastok["n_recv_errors"])
        if rc + re_:
            print(f"  rx errors {re_} of {rc+re_} ({100.0*re_/(rc+re_):.1f}%)   "
                  f"noise {lastok['noise_floor']} dBm")

    # a lost flood is normal; a run of them is not
    run = best = 0
    for r in todays:
        run = run + 1 if r["ok"] != "yes" else 0
        best = max(best, run)
    if best >= 3:
        print(f"  NOTE: {best} consecutive misses — prod may have been unreachable")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--interval", type=float, default=900.0, help="seconds (default 900)")
    ap.add_argument("--csv", default=DEFAULT_CSV)
    ap.add_argument("--host", default="192.168.88.69")
    ap.add_argument("--panel-password", default="password")
    ap.add_argument("--key", default=PROD_KEY)
    ap.add_argument("--wait", type=float, default=45.0)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--report", action="store_true", help="summarise a day and exit")
    ap.add_argument("--day", help="YYYY-MM-DD for --report (default today, local)")
    args = ap.parse_args()

    if args.report:
        sys.exit(report(args.csv, args.day))

    admin_pw = os.environ.get("MESHCORE_PROD_PASSWORD", "")
    if not admin_pw:
        raise SystemExit("set MESHCORE_PROD_PASSWORD (prod's admin password)")

    ma = load_mesh_admin()
    fresh = not os.path.exists(args.csv)
    fh = open(args.csv, "a", newline="")
    w = csv.DictWriter(fh, fieldnames=FIELDS)
    if fresh:
        w.writeheader()
        fh.flush()

    print(f"prod_battery_watch: every {args.interval/60:.0f} min -> {args.csv}")
    while True:
        row = poll_once(ma, args.host, args.panel_password, args.key.lower(),
                        admin_pw, args.wait)
        if row["ok"] == "yes":
            print(f"  {row['utc']}  {row['battery_mv']} mV   up {int(row['uptime_s'])/86400:.2f} d",
                  flush=True)
        else:
            print(f"  {row['utc']}  MISS: {row['error']}", flush=True)
        w.writerow(row)
        fh.flush()
        if args.once:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
