#!/usr/bin/env python3
"""Manage a remote MeshCore repeater over the mesh, through the M5's companion.

The M5 runs several identities on one radio. Its repeater keeps doing its own
job; this drives the COMPANION identity — the same one the phone app talks to —
to log in to another repeater over LoRa and run CLI commands on it. That is the
only way to reach a node with no USB and no BLE range.

    tools/mesh_admin.py contacts
    tools/mesh_admin.py add   --key 3070D00B... --name VIC-NorthcoteNW-DIS
    tools/mesh_admin.py login --key 3070D00B... --password <admin-password>
    tools/mesh_admin.py cmd   --key 3070D00B... "stats-core"

Login is per-session on the far node, so `login` before `cmd`. A command sent
without a valid login is silently ignored by the repeater rather than refused,
which looks identical to being out of range — hence login reports explicitly.

Frames are the stock companion protocol (examples/companion_radio/MyMesh.cpp),
tunnelled through the panel's /api/multi/comp/frame endpoint:
    9  CMD_ADD_UPDATE_CONTACT  [9][pub32][type][flags][path_len][path64][name32][adv_ts4]
    26 CMD_SEND_LOGIN          [26][pub32][password]
    2  CMD_SEND_TXT_MSG        [2][txt_type][attempt][ts4][pub_prefix6][text]
       txt_type 1 = TXT_TYPE_CLI_DATA
"""
import argparse
import json
import ssl
import struct
import sys
import time
import urllib.request

CONTACT_TYPE_REPEATER = 2
TXT_TYPE_CLI_DATA = 1
PATH_LEN_UNKNOWN = 0xFF          # no route known: send by flood

CTX = ssl._create_unverified_context()


def http(host, path, token=None, body=None, ctype=None, timeout=30):
    req = urllib.request.Request(f"https://{host}{path}", data=body,
                                 method="POST" if body is not None else "GET")
    if token:
        req.add_header("X-Auth-Token", token)
    if ctype:
        req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
        return r.read()


def login_panel(host, password):
    tok = http(host, "/login", body=password.encode(), timeout=20).decode().strip()
    if len(tok) != 32:
        raise SystemExit(f"panel login failed: {tok[:80]!r}")
    return tok


def send_frame(host, token, frame, timeout=30):
    """POST one companion frame; returns the list of reply frames."""
    raw = http(host, "/api/multi/comp/frame", token, bytes(frame),
               "application/octet-stream", timeout)
    out, o = [], 0
    while o + 2 <= len(raw):
        ln = raw[o] | (raw[o + 1] << 8)
        o += 2
        if ln == 0 or o + ln > len(raw):
            break
        out.append(raw[o:o + ln])
        o += ln
    return out


def app_start(host, token, app="MeshAdmin"):
    """The companion ignores commands until an app has introduced itself.

    Same two frames the panel sends: DEVICE_QUERY at protocol v3, then
    APP_START with seven reserved bytes and the app name. Skipping this is
    indistinguishable from the far node being out of range — the commands are
    simply dropped.
    """
    send_frame(host, token, [22, 3])
    f = bytearray([1]) + bytes(7) + app.encode()[:16]
    return send_frame(host, token, f)


def get_contacts(host, token):
    contacts = []
    for f in send_frame(host, token, [4]):
        if f[0] == 3 and len(f) >= 136:
            contacts.append({
                "pub": f[1:33].hex(),
                "type": f[33],
                "path_len": f[35],
                "name": f[100:132].split(b"\0")[0].decode("utf-8", "replace"),
            })
    return contacts


def add_contact(host, token, pub_hex, name, ctype=CONTACT_TYPE_REPEATER):
    pub = bytes.fromhex(pub_hex)
    if len(pub) != 32:
        raise SystemExit("public key must be 32 bytes (64 hex chars)")
    f = bytearray([9])
    f += pub
    f += bytes([ctype, 0, PATH_LEN_UNKNOWN])
    f += bytes(64)                                  # out_path, unused
    f += name.encode("utf-8")[:32].ljust(32, b"\0")
    f += struct.pack("<I", int(time.time()))        # last_advert_timestamp
    return send_frame(host, token, f)


def archive_after(host, token, after):
    """[u32 latest][u32 seq][u16 len][frame]... — returns (latest, [frames])."""
    raw = http(host, f"/api/multi/comp/archive?after={after}", token)
    if len(raw) < 4:
        return after, []
    latest = struct.unpack("<I", raw[:4])[0]
    frames, o = [], 4
    while o + 6 <= len(raw):
        ln = raw[o + 4] | (raw[o + 5] << 8)
        f = raw[o + 6:o + 6 + ln]
        o += 6 + ln
        if f:
            frames.append(f)
    return latest, frames


def wait_for(host, token, after, codes, secs):
    """Watch the push mirror for any of `codes`; returns (code, frame) or None."""
    deadline = time.time() + secs
    cursor = after
    while time.time() < deadline:
        cursor, frames = archive_after(host, token, cursor)
        for f in frames:
            if f and f[0] in codes:
                return f[0], f
        time.sleep(1.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["contacts", "add", "login", "cmd", "status"])
    ap.add_argument("text", nargs="*", help="CLI command text, for `cmd`")
    ap.add_argument("--host", default="192.168.88.69")
    ap.add_argument("--panel-password", default="password")
    ap.add_argument("--key", help="the far node's 64-hex public key")
    ap.add_argument("--name", default="", help="contact name, for `add`")
    ap.add_argument("--password", default="", help="the far node's ADMIN password, for `login`")
    ap.add_argument("--wait", type=float, default=25.0, help="seconds to wait for a reply")
    args = ap.parse_args()

    tok = login_panel(args.host, args.panel_password)
    app_start(args.host, tok)

    if args.action == "contacts":
        cs = get_contacts(args.host, tok)
        print(f"{len(cs)} contact(s)")
        for c in cs:
            route = "flood" if c["path_len"] == PATH_LEN_UNKNOWN else f"{c['path_len']} hop"
            print(f"  {c['pub'][:16]}  type={c['type']}  {route:<7}  {c['name']!r}")
        return

    if not args.key:
        raise SystemExit("--key is required")
    key = args.key.lower().replace(" ", "")

    if args.action == "add":
        add_contact(args.host, tok, key, args.name)
        got = [c for c in get_contacts(args.host, tok) if c["pub"] == key]
        print(f"added: {got[0]['name']!r} {got[0]['pub'][:16]}" if got
              else "add did NOT take — contact not in list afterwards")
        return

    latest, _ = archive_after(args.host, tok, 0)

    if args.action == "login":
        f = bytearray([26]) + bytes.fromhex(key) + args.password.encode()
        send_frame(args.host, tok, f)
        # 0x85 = login success, 0x86 = rejected (see the panel's room-join flow)
        hit = wait_for(args.host, tok, latest, {0x85, 0x86}, args.wait)
        if not hit:
            print("no answer — out of range, wrong key, or the node never replied")
            sys.exit(2)
        print("login OK" if hit[0] == 0x85 else "login REJECTED — wrong admin password")
        sys.exit(0 if hit[0] == 0x85 else 3)

    if args.action == "status":
        # CMD_SEND_STATUS_REQ. This is what the phone app uses, and unlike the
        # CLI it returns a LIVE battery reading -- getBatteryMilliVolts(true) on
        # the far node -- rather than a value latched at boot. Guests can call
        # it too, so it works even without admin rights.
        if args.password:
            send_frame(args.host, tok, bytearray([26]) + bytes.fromhex(key) + args.password.encode())
            wait_for(args.host, tok, latest, {0x85, 0x86}, 20)
            latest, _ = archive_after(args.host, tok, latest)
        send_frame(args.host, tok, bytearray([27]) + bytes.fromhex(key))
        hit = wait_for(args.host, tok, latest, {0x87}, args.wait)
        if not hit:
            print("no status reply (out of range, or the node never answered)")
            sys.exit(2)
        f = hit[1]
        # [0]=0x87 [1]=reserved [2:8]=key prefix [8:]=RepeaterStats, LE
        v = struct.unpack_from("<HHhhIIIIIIIIHhHHII", f, 8)
        (batt, txq, noise, rssi, precv, psent, air, up, sflood, sdirect,
         rflood, rdirect, errs, snr4, ddups, fdups, rxair, rerrs) = v
        pct_err = (100.0 * rerrs / (precv + rerrs)) if (precv + rerrs) else 0.0
        print(f"  battery      {batt} mV        (live)")
        print(f"  uptime       {up} s ({up/86400:.2f} d)")
        print(f"  noise floor  {noise} dBm      last rssi {rssi} dBm  last snr {snr4/4:.2f} dB")
        print(f"  packets      recv {precv}  sent {psent}  recv_errors {rerrs} ({pct_err:.1f}%)")
        print(f"  flood/direct sent {sflood}/{sdirect}  recv {rflood}/{rdirect}")
        print(f"  airtime      tx {air} s  rx {rxair} s   tx queue {txq}   err_events {errs}")
        print(f"  duplicates   direct {ddups}  flood {fdups}")
        return

    if args.action == "cmd":
        text = " ".join(args.text)
        if not text:
            raise SystemExit("give a command, e.g. `cmd --key ... \"stats-core\"`")
        # A repeater login is a SESSION on the far node and lapses, so a cmd
        # issued minutes after a separate `login` silently does nothing. Given
        # a password, log in again here so the pair is atomic.
        if args.password:
            send_frame(args.host, tok, bytearray([26]) + bytes.fromhex(key) + args.password.encode())
            if not wait_for(args.host, tok, latest, {0x85, 0x86}, 20):
                print("login: no answer — wrong password or out of range")
                sys.exit(2)
            latest, _ = archive_after(args.host, tok, latest)
        pub = bytes.fromhex(key)
        f = bytearray([2, TXT_TYPE_CLI_DATA, 0])
        f += struct.pack("<I", int(time.time()))
        f += pub[:6]                                 # recipient by key prefix
        f += text.encode()
        send_frame(args.host, tok, f)
        print(f"sent: {text}")
        # The reply is a normal contact message the node holds until asked
        # for it, so pull with SYNC_NEXT_MESSAGE rather than waiting on a push.
        # Frame 16 = contact msg v3: [0]=16 [1]=snr*4 [4:10]=key prefix
        # [12:16]=timestamp [16:]=text. A long CLI reply arrives as several.
        deadline = time.time() + args.wait
        want, got, seen = key[:12], False, set()
        while time.time() < deadline:
            drained = False
            for f in send_frame(args.host, tok, [10]):
                if f[0] == 16 and len(f) >= 16:
                    drained = True
                    prefix = f[4:10].hex()
                    body = f[16:].decode("utf-8", "replace").rstrip("\0")
                    if prefix == want:
                        if body not in seen:      # the same reply arrives twice
                            seen.add(body)
                            print(f"  {body}")
                        got = True
                    else:
                        print(f"  (from {prefix[:8]}) {body}")
            if not drained:
                time.sleep(1.5)
        if not got:
            print("  (no reply -- not logged in, wrong password, or out of range)")


if __name__ == "__main__":
    main()
