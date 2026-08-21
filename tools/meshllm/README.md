# meshllm — an LLM on the mesh

Answers messages prefixed with `@` in the node's **room** and in **DMs** to its
companion identity, using a local model on llmswarm. Replies are flattened to
one line and hard-clipped to the 140-byte mesh message budget.

```
room post / DM starting with "@"   ->   llmswarm (Qwen3.6-35B-A3B)
   ->  flatten, clip to 140 bytes  ->   posted back into the room / DM reply
```

Host-side Ruby, stdlib only (tested on macOS system Ruby 2.6). No firmware
changes: it drives the existing companion identity through the web panel's
frame mux, so it shares the companion with a live phone app instead of kicking
it off the way a raw TCP `:5000` client would.

## Setup

```bash
cp config.example.json config.json      # then edit: node.host, passwords
./meshllm.rb selftest                   # frame codec + shaping, no network
./meshllm.rb probe                      # node + LLM reachability, room, limits
./meshllm.rb ask "what is a spreading factor?"   # LLM path only, no radio
./meshllm.rb run                        # the bridge
./meshllm.rb say "@test question"       # post into the room (live round-trip check)
```

`run` stays in the foreground and logs to stderr. To leave it up:

```bash
nohup ./meshllm.rb run >> ~/.meshllm/meshllm.log 2>&1 &
```

`MESHLLM_HOST` and `MESHLLM_PASSWORD` override `node.host` / `node.password` if
you would rather not keep them in the file.

### Choosing the room

`probe` lists every room the companion knows and marks which one is this box's
own room identity:

```
rooms     :
            57fc8dd0e647 3156_Room
            6d79d4a5d3eb Castle          <- configured
```

Set `room.name` to one of them — by name, or by pubkey prefix when two rooms
share a name (this box's own room and a remote one often do). The bridge
refuses to start rather than guess.

`room.source` is `auto` by default and follows that choice:

- the room **is** this box's own identity → read its posts over HTTP from
  `/api/multi/room/posts`, no login needed to see them
- the room is **remote** → read the posts it pushes to us over RF, which
  requires being logged in

Either way the companion needs the room as a **contact** and, to post, a login
with write permission. For this box's own room:

```bash
./meshllm.rb pair-room     # reads the room's key off the device with
                           # `room get public.key` and adds the contact
                           # directly -- nothing is transmitted, so a room
                           # with advert.interval 0 stays unannounced
```

For a remote room the contact appears on its own when it adverts. Then set
`room.password` to the room's password — the guest/room password grants
`PERM_ACL_READ_WRITE`, and a wrong one silently downgrades you to read-only, so
the symptom of a bad password is "receives nothing, posts vanish". Alternatively
add the companion's pubkey to the room ACL (`room setperm <pubkey> <perm>` in
the panel console) and leave the password blank.

`./meshllm.rb console "<cmd>"` runs any panel console command (`identities`,
`room get name`, `room get acl`) if you need to check from here.

## How it works

| Direction | Mechanism |
| --- | --- |
| Room in | `GET /api/multi/room/posts` (the room's 32-slot post queue — no login needed) and/or room pushes that arrive at the logged-in companion as signed messages |
| Room out | a plain text message to the room server, which stores it and pushes it to every other logged-in client — i.e. the bot posts as a normal room member |
| DM in | contact messages mirrored into the companion's archive (`GET /api/multi/comp/archive`), plus a queue drain when no phone app is attached |
| DM out | `CMD_SEND_TXT_MSG` to the sender's pubkey prefix |

`room.source` overrides the automatic choice: `posts`, `push`, or `both`.
Duplicates across the two paths are collapsed by `(timestamp, text)`. The posts
endpoint is only ever read when the configured room *is* this box's own, so a
local post can never be answered into a remote room.

Draining the device's offline queue is destructive — a phone app would never see
what we pull — so the bridge only drains while `companion.client` is false.

The mux serves one exchange at a time, and a phone app syncing 160 contacts can
hold it for seconds, so startup retries `APP_START`/`GET_CONTACTS` with widening
windows instead of treating an empty answer as failure. Contacts are re-read
every `contacts_refresh_minutes` because new nodes advert constantly.

## Triggering

- Any room post or DM whose text starts with `@` (configurable `trigger`).
- Set `bot_name` to also accept `@name <question>` and strip the name.
- `@!help`, `@!status`, `@!reset` (forget the conversation history).

Conversation history is kept per scope (the room shares one thread; each DM
correspondent gets its own), `history_turns` deep, expiring after
`history_idle_minutes`, persisted in `state_file` so a restart keeps context.

## Keeping replies short

Three layers, because a model asked nicely is not a guarantee:

1. A system prompt stating the hard character limit, one sentence, no markdown.
2. `max_tokens` (default 96) so a runaway answer costs little time.
3. `Shaper` — strips markdown/newlines to one line, then clips on a **byte**
   boundary (never mid-codepoint) to `reply_limit`.

`max_chunks` defaults to 1 (one message per question). Raise it to allow
`1/2`-prefixed continuations, sent `chunk_gap_seconds` apart to be kind to
airtime.

Limits worth knowing: a DM tops out at `MAX_TEXT_LEN` = 160 bytes, and the room
clamps a stored post to `MAX_POST_TEXT_LEN` = 151. `reply_limit` is capped to
151 automatically; 140 leaves margin.

## Thinking models

Qwen3.6 is a reasoning model. Left alone it spends the whole token budget in
`reasoning_content` and returns **empty** `content` — so the bridge sends
`chat_template_kwargs: {enable_thinking: false}`, which llama.cpp passes to the
chat template. Measured on this box: 0.6–1.3 s per answer with thinking off
versus 27 s and no usable answer with it on. Set `llm.thinking: true` to opt
back in (and raise `max_tokens` and `timeout_seconds` if you do). If content
ever does come back empty, the bridge salvages the last sentence of the
reasoning rather than replying with nothing.

## Rate limits

`limits` caps per-sender interval and hourly count, a global hourly count, and
the pending queue depth (LLM calls are serialized on one worker). A room post's
author is a real pubkey prefix, so per-sender limits are meaningful there; the
gate on *who* can ask is room membership itself.

## Operational notes

- Posts authored by this companion are **not** skipped: a phone app driving the
  same identity is the operator's own way into the room. Loops are prevented at
  the other end — an outgoing reply can never start with the trigger.
- Backend errors reach the mesh as one short sentence ("sorry, the model backend
  is having a moment"), never as clipped JSON; the detail goes to the log. A 5xx
  is retried once, since a reloading backend usually recovers.
- If the model host is up but a specific model 500s with `Compute error.`, that
  is the backend, not the bridge — `curl localhost:50080/v1/models` and try
  another model to confirm.

## Files

- `meshllm.rb` — the whole bridge (`run`, `probe`, `pair-room`, `say`, `console`,
  `ask`, `shape`, `selftest`)
- `config.example.json` — every setting with its default
- state lives in `~/.meshllm/state.json` (archive cursor, newest answered post,
  dedup ring, per-scope history)
