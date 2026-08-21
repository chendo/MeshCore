#!/usr/bin/env python3
"""Serve the web panel from the working tree, against a live device.

The panel is ~120 KB of HTML/JS compiled into the firmware, so trying a layout
tweak used to cost a 35 s build plus a 20 s flash — and on a node that is doing
real work, a reboot as well. This serves the page straight off disk and
forwards everything else — /login, /api/**, the lot — to a real node over
HTTPS, so what runs in the browser is the working copy while the data behind it
is the device's own.

    uv run python tools/panel_dev.py --device 192.168.88.69

Open the printed URL and log in exactly as you would on the device; the token
the page gets back is the device's, because the login went there. A reload
picks up edits — no build, no flash, no reboot.

The page is re-read per request, and the C++ raw-string wrapper is stripped the
same way tools/gzip_panel.py does it, so what you are looking at is the literal
that would be compiled in, not a copy that can drift.

Firmware upload is refused unless --allow-ota: an OTA POSTed by accident from a
half-edited page is a bad afternoon.
"""

import argparse
import re
import socket
import ssl
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PAGE = Path(__file__).resolve().parent.parent / "examples/multi_node/web_multi_page.h"
# Headers worth carrying in either direction. Everything else (Host, cookies,
# encodings) is the proxy's business, not the page's.
FORWARD = ("X-Auth-Token", "X-Firmware-MD5", "Content-Type")


def page_html(path):
    text = path.read_text(encoding="utf-8")
    m = re.search(r'R"MWPG\((.*)\)MWPG"', text, re.S)
    if not m:
        raise SystemExit(f'{path}: could not find the R"MWPG(...)MWPG" page literal')
    return m.group(1).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    device = ""
    allow_ota = False
    ctx = None

    def log_message(self, fmt, *args):        # one line per request, not three
        sys.stderr.write("  %s\n" % (fmt % args))

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/?"):
            try:
                body = page_html(PAGE)
            except SystemExit as e:
                return self.send_error(500, str(e))
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        self.proxy("GET")

    def do_POST(self):
        if "firmware-update" in self.path and not self.allow_ota:
            return self.send_error(
                403, "firmware upload refused by panel_dev (pass --allow-ota to permit it)")
        self.proxy("POST")

    def proxy(self, method):
        body = None
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            body = self.rfile.read(n)
        req = urllib.request.Request(f"https://{self.device}{self.path}", data=body, method=method)
        for h in FORWARD:
            if self.headers.get(h) is not None:
                req.add_header(h, self.headers[h])
        try:
            with urllib.request.urlopen(req, timeout=30, context=self.ctx) as r:
                status, payload = r.status, r.read()
                ctype = r.headers.get("Content-Type", "application/octet-stream")
        except urllib.error.HTTPError as e:            # 401 and friends are answers too
            status, payload = e.code, e.read()
            ctype = e.headers.get("Content-Type", "text/plain")
        except Exception as e:                          # device asleep, wifi gone, cert odd
            status, payload, ctype = 502, str(e).encode(), "text/plain"
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", required=True, help="node's IP or hostname (HTTPS, self-signed)")
    ap.add_argument("--port", type=int, default=8713)
    # Every machine on the LAN by default, because testing the panel on a phone
    # or a second desktop is most of the point of it. It is still a dev proxy:
    # it holds no credentials, so a visitor faces the node's own login.
    ap.add_argument("--bind", default="0.0.0.0", help="interface to listen on (default: all)")
    ap.add_argument("--allow-ota", action="store_true", help="permit /api/firmware-update through")
    args = ap.parse_args()

    Handler.device = args.device
    Handler.allow_ota = args.allow_ota
    # the node serves its own certificate; this is a LAN dev proxy, not a
    # security boundary, and the alternative is pinning a cert that changes
    Handler.ctx = ssl._create_unverified_context()

    page_html(PAGE)   # fail now, not on the first request
    # flushed, because this banner is worth having when stdout is a log file
    for host in listen_urls(args.bind, args.device):
        print(f"panel_dev: http://{host}:{args.port}  ->  https://{args.device}", flush=True)
    print(f"           serving {PAGE.relative_to(Path.cwd()) if PAGE.is_relative_to(Path.cwd()) else PAGE}"
          " — http, not https; reload the browser to pick up edits", flush=True)
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


def listen_urls(bind, device):
    """Addresses worth printing — the LAN one is the whole reason for --bind."""
    if bind not in ("0.0.0.0", "::"):
        return [bind]
    hosts = ["127.0.0.1"]
    try:
        # no packets are sent; this just asks the routing table which local
        # address would be used to reach the node, which is the one a second
        # machine on that network can reach us on too
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((device, 443))
        hosts.append(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    return hosts


if __name__ == "__main__":
    main()
