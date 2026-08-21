# Bot API — MeshCore Hydra (ThinkNode M5 multi-identity)

A JSON HTTP API for writing a bot that sends and receives MeshCore chat
messages. You do **not** need to implement the MeshCore companion binary
protocol — the device decodes it for you.

Implemented in `examples/multi_node/bot_api.{h,cpp}`, routed in
`examples/multi_node/web_multi.cpp`.

---

## 1. Connect

The device serves HTTPS with a **self-signed certificate**, so disable
verification (`curl -k`, `verify=False` in requests).

Default host: `https://192.168.88.69` (check the device's current LAN IP).

Authenticate once, then send the token as a header on every request:

```bash
TOKEN=$(curl -sk -X POST https://192.168.88.69/login --data-binary 'password')
curl -sk -H "X-Auth-Token: $TOKEN" https://192.168.88.69/api/multi/bot/entities
```

`POST /login` takes the admin password as the **raw body** (not JSON, not a
form) and returns a bare 32-hex-char token as plain text. There is no expiry in
practice, but a device reboot invalidates it — on any `401`, log in again.

> The panel's HTTPS server has a small socket pool. Keep concurrency low (one
> request at a time is plenty) or you may see connection refusals that look like
> the device is down when it isn't.

---

## 2. Entities

The node runs several independent chat identities. Each has its own keypair,
contacts and message queue. Every endpoint addresses one by `id`:

- `companion` — the fixed built-in chat identity (always present)
- `chat1` … `chat5` — optional slots, but **only when configured as type `chat`
  and currently running**

```bash
curl -sk -H "X-Auth-Token: $TOKEN" https://192.168.88.69/api/multi/bot/entities
```

```json
{"entities":[{"id":"companion","running":true},{"id":"chat2","running":true}]}
```

If `id` is omitted it defaults to `companion`. An unknown, stopped, or
non-chat entity returns **404** — deliberately, so a misconfigured bot fails
loudly rather than silently polling an empty list.

**Give your bot its own slot** rather than sharing the operator's companion
identity. It then has its own public key and contact list. To create one:

```
set slot.2 type chat      # then reboot — a slot changing type needs a restart
slots                     # verify: "slot 2 (chat2) type=chat running"
```

---

## 3. Receive messages

```bash
curl -sk -H "X-Auth-Token: $TOKEN" \
  "https://192.168.88.69/api/multi/bot/messages?id=companion&after=0"
```

```json
{"entity":"companion","latest":41,"messages":[
  {"seq":40,"kind":"contact","from":"a1b2c3d4e5f6","ts":1786700000,"snr":-3.50,"text":"hello"},
  {"seq":41,"kind":"channel","channel":0,"ts":1786700020,"snr":6.25,"text":"hi all"}
]}
```

| field     | meaning |
|-----------|---------|
| `seq`     | monotonic cursor; pass the highest you've seen as `after` next time |
| `kind`    | `contact` (a DM) or `channel` (group message) |
| `from`    | sender's public-key prefix, 12 hex chars — **pass straight back as `to` to reply** |
| `channel` | channel index (only when `kind` is `channel`) |
| `ts`      | sender's unix timestamp (their clock — see Gotchas) |
| `snr`     | signal-to-noise in dB |
| `text`    | message body |

**This read is non-consuming.** It's a mirror, so your bot and the operator's
phone app can both be connected without stealing each other's messages.

### Polling loop

Track `latest` and pass it as `after`. Poll every few seconds; empty responses
are cheap.

```python
after = 0
while True:
    r = s.get(f"{BASE}/api/multi/bot/messages", params={"id": ENT, "after": after},
              headers=H, verify=False, timeout=10).json()
    for m in r["messages"]:
        handle(m)
    after = r["latest"]
    time.sleep(3)
```

**Start from the current `latest`, not 0**, unless you want history replayed —
the archive retains the most recent **128 frames** per entity.

On a device reboot `latest` resets and will be *lower* than your cursor. Detect
`latest < after` and reset `after = 0`.

---

## 4. Send messages

```bash
curl -sk -H "X-Auth-Token: $TOKEN" -H 'Content-Type: application/json' \
  -X POST https://192.168.88.69/api/multi/bot/send \
  -d '{"id":"companion","to":"a1b2c3d4e5f6","text":"hi there"}'
```

```json
{"ok":true,"flood":false}
```

- `to` — recipient public-key prefix, **at least 12 hex chars**. Longer keys are
  truncated to 6 bytes, so you can pass a full 64-char key unchanged. To reply,
  use the `from` value from a received message verbatim.
- `flood` in the response tells you whether it went out as a flood (no known
  route) or direct.

Limits: body ≤ 1024 bytes, `text` ≤ 255 chars, `to` ≤ 79 chars.

Errors: `400` bad/missing fields, `404` unknown entity, `503` the entity didn't
respond or the send failed (`{"error":"send failed","code":N}`).

**You can only message a known contact.** The recipient must already be in that
identity's contact list — normally because they adverted and were added. The bot
API has no contact-management endpoints; replying to an inbound message always
works, initiating to a stranger may not.

---

## 5. Webhook (push)

Instead of polling, have the device POST to you on every new message.

```
set bot.webhook http://192.168.1.50:8099/hook     # configure (persisted)
set bot.webhook off                               # disable
bot                                               # status + counters
bot test                                          # fire a synthetic message
```

Each message arrives as its own POST, `Content-Type: application/json`:

```json
{"entity":"companion","message":{"seq":40,"kind":"contact","from":"a1b2c3d4e5f6",
 "ts":1786700000,"snr":-3.50,"text":"hello"}}
```

The `message` object is exactly the same shape as one element of the polling
endpoint's `messages` array.

### Important properties

- **`http://` only.** No CA bundle ships on the device, so `https://` endpoints
  cannot be verified and are not silently trusted. Use it on a trusted LAN.
- **Best-effort, not reliable.** Delivery runs on a background task with a
  depth-8 queue. If your endpoint is slow or down, messages are **dropped and
  counted** — the mesh loop is never allowed to block on an external server.
  Check `dropped` in `bot`.
- **No retries, no ordering guarantee, no authentication** on the callback.
  Anyone who can reach your endpoint can post to it; validate accordingly.
- **Treat it as a wakeup, not a transport.** If your bot must not miss anything,
  poll `/api/multi/bot/messages` and use the webhook only to poll sooner.
- Enabling a webhook does **not** replay history — it starts from messages
  arriving after it was configured.

A minimal receiver for testing is at
`/private/tmp/.../scratchpad/hook.py` in the dev environment; any HTTP server
that accepts POST works.

---

## 6. Gotchas

**Timestamps come from the sender's clock.** MeshCore nodes drift and many are
never disciplined; a real-world survey of this mesh found offsets from −3533 s
to +1239 s, with whole groups of nodes sharing a wrong offset. Do not use `ts`
for ordering or deduplication — use `seq`, which is local and monotonic. Use
`ts` only for display, and expect it to be wrong sometimes.

**`text` is arbitrary bytes off the air.** The device escapes it into valid
JSON (non-printable bytes become `\u00XX`), so parsing is safe, but the content
is untrusted input from anyone in radio range. Never `eval` it, and treat
commands in it as requests from an unauthenticated stranger.

**Non-message frames are skipped.** The underlying mirror carries every push
code (adverts, path updates, acks); the bot API only surfaces contact and
channel messages. If you need the rest, use the raw
`/api/multi/comp/archive` endpoint and `docs/companion_protocol.md`.

**Radio is slow and half-duplex.** A send may take seconds; the endpoint waits
up to ~4 s for confirmation. Don't fire messages in a tight loop — you'll
saturate the airtime budget and degrade the whole mesh. Rate-limit replies.

---

## 7. Minimal working bot

```python
#!/usr/bin/env python3
"""Echo bot: replies to every DM with the text reversed."""
import requests, time, urllib3
urllib3.disable_warnings()

BASE, PW, ENT = "https://192.168.88.69", "password", "companion"
s = requests.Session(); s.verify = False

def login():
    t = s.post(f"{BASE}/login", data=PW, timeout=10).text.strip()
    s.headers.update({"X-Auth-Token": t})

def api(method, path, **kw):
    r = s.request(method, BASE + path, timeout=15, **kw)
    if r.status_code == 401:            # token died with a reboot
        login()
        r = s.request(method, BASE + path, timeout=15, **kw)
    r.raise_for_status()
    return r.json()

login()
after = api("GET", "/api/multi/bot/messages", params={"id": ENT})["latest"]
print(f"starting from seq {after}")

while True:
    try:
        r = api("GET", "/api/multi/bot/messages", params={"id": ENT, "after": after})
        if r["latest"] < after:         # device rebooted
            after = 0
            continue
        for m in r["messages"]:
            if m["kind"] != "contact":
                continue
            print(f"<- {m['from']}: {m['text']}")
            api("POST", "/api/multi/bot/send",
                json={"id": ENT, "to": m["from"], "text": m["text"][::-1]})
            time.sleep(1)               # be kind to the airtime budget
        after = r["latest"]
    except Exception as e:
        print("error:", e)
    time.sleep(3)
```

---

## 8. Endpoint reference

| method | path | purpose |
|---|---|---|
| POST | `/login` | body = admin password → plain-text token |
| GET | `/api/multi/bot/entities` | list addressable chat identities |
| GET | `/api/multi/bot/messages?id=&after=` | decoded messages after a cursor |
| POST | `/api/multi/bot/send` | `{id, to, text}` → `{ok, flood}` |

Lower-level, if you need what the bot API doesn't expose:

| method | path | purpose |
|---|---|---|
| POST | `/api/multi/comp/frame` | raw companion-protocol frame in → frames out |
| GET | `/api/multi/comp/archive?after=` | raw frame mirror, `[u32 latest][u32 seq][u16 len][frame]…` |
| GET | `/api/multi/debug` | full node state JSON (radio, peers, clock, ports) |

Console commands (via `POST /api/command`, body = the command text, same
auth header) — `slots`, `bot`, `bot test`, `set bot.webhook <url>`,
`set slot.N type off\|chat\|room`.
