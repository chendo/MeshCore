#!/usr/bin/env bash
# Post a message into the room, via the on-board companion identity (which must
# already be a room member — join once from the panel's Room tab).
#
#   ./roompost.sh "hello room"
#
# Frame is CMD_SEND_TXT_MSG:
#   [0x02][txt_type=0][attempt=0][u32 LE unix time][6-byte dest pubkey prefix][text]
set -euo pipefail

HOST=${HOST:-192.168.88.69}
PASS=${PASS:-password}
ROOM_PREFIX=${ROOM_PREFIX:-c1a551f1eda4}     # first 6 bytes of the room pubkey
TEXT=${1:?usage: roompost.sh "<message>"}

TOKEN=$(curl -sk --max-time 10 -X POST "https://$HOST/login" --data-binary "$PASS")
[ -n "$TOKEN" ] || { echo "login failed"; exit 1; }

# build the binary frame
python3 - "$ROOM_PREFIX" "$TEXT" > /tmp/roomframe.bin <<'PY'
import sys, time, struct
prefix, text = sys.argv[1], sys.argv[2]
sys.stdout.buffer.write(
    bytes([0x02, 0x00, 0x00]) + struct.pack("<I", int(time.time()))
    + bytes.fromhex(prefix) + text.encode())
PY

curl -sk --max-time 30 -X POST "https://$HOST/api/multi/comp/frame?t=8000&i=600" \
  -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/octet-stream" \
  --data-binary @/tmp/roomframe.bin \
| python3 -c '
import sys
buf = sys.stdin.buffer.read(); o = 0
while o + 2 <= len(buf):
    l = buf[o] | (buf[o+1] << 8); f = buf[o+2:o+2+l]; o += 2 + l
    if not f: continue
    if f[0] == 6:
        print("sent (%s)" % ("flood" if len(f) > 1 and f[1] else "direct")); sys.exit(0)
    if f[0] == 1:
        print("send failed, err %s" % (f[1] if len(f) > 1 else "?")); sys.exit(1)
    print("frame code 0x%02x" % f[0])
print("no send confirmation")
'
