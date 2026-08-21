#include "bot_api.h"
#include "multi_web.h"
#include "slot_types.h"
#include <Utils.h>
#include <target.h>                        // rtc_clock
#include <helpers/BaseSerialInterface.h>   // MAX_FRAME_SIZE
#include <Preferences.h>

// ---- companion protocol bits we decode ------------------------------------
// Documented in docs/companion_protocol.md; the offsets are fixed by the
// protocol, so they are named here rather than repeated as magic numbers.
#define RESP_CONTACT_MSG   16     // [0]=16 [1]=snr*4 [4..10]=pubkey prefix [12..16]=u32 ts [16..]=text
#define RESP_CHANNEL_MSG   17     // [0]=17 [1]=snr*4 [4]=channel [7..11]=u32 ts  [11..]=text
#define RESP_SENT           6     // [1] = 1 when it went out as a flood
#define RESP_ERR            1
#define CMD_SEND_TXT        2

#define CONTACT_MSG_HDR    16
#define CHANNEL_MSG_HDR    11
#define PUBKEY_PREFIX_LEN   6

static uint32_t rdU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- entities --------------------------------------------------------------

bool botResolveEntity(const char* id, int* slot) {
  if (id == nullptr || *id == 0) return false;
  if (strcmp(id, "companion") == 0) {
    if (slot) *slot = -1;
    return true;   // the fixed companion is always constructed
  }
  if (strncmp(id, "chat", 4) != 0) return false;
  int n = atoi(id + 4) - 1;
  if (n < 0 || n >= MULTI_MAX_CHAT_SLOTS) return false;
  // Only a slot actually RUNNING as a chat identity can carry messages. A slot
  // that is off, or is running as a room, has no companion-protocol queue at
  // all — saying "unknown entity" there is clearer than an empty message list.
  if (slotTypeGet(n) != SLOT_CHAT || !multiChatSlotRunning(n)) return false;
  if (slot) *slot = n;
  return true;
}

int botEntitiesJson(char* out, size_t cap) {
  size_t o = 0;
  o += snprintf(out + o, cap - o, "{\"entities\":[{\"id\":\"companion\",\"running\":true}");
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS && o + 64 < cap; i++) {
    if (slotTypeGet(i) != SLOT_CHAT) continue;
    o += snprintf(out + o, cap - o, ",{\"id\":\"chat%d\",\"running\":%s}",
                  i + 1, multiChatSlotRunning(i) ? "true" : "false");
  }
  o += snprintf(out + o, cap - o, "]}");
  return (int)o;
}

// per-entity archive access
static uint32_t entArchiveSeq(int slot) {
  return slot < 0 ? compArchiveSeq() : multiChatSlotArchiveSeq(slot);
}
static int entArchiveCopy(int slot, uint32_t after, uint8_t* out, size_t cap) {
  return slot < 0 ? compArchiveCopy(after, out, cap) : multiChatSlotArchiveCopy(slot, after, out, cap);
}
static int entFrameExchange(int slot, const uint8_t* f, size_t len, uint8_t* out, size_t cap,
                            uint32_t total_ms, uint32_t idle_ms) {
  return slot < 0 ? compWebFrameExchange(f, len, out, cap, total_ms, idle_ms)
                  : multiChatSlotFrameExchange(slot, f, len, out, cap, total_ms, idle_ms);
}

// ---- JSON emit helpers -----------------------------------------------------

// Message text is arbitrary bytes off the air. It goes into JSON, so it must be
// escaped, and anything that is not printable ASCII is emitted as \u00XX rather
// than trusted to be valid UTF-8 — a truncated multi-byte sequence at the end of
// a LoRa frame would otherwise produce JSON no parser will accept.
static size_t jsonStr(char* out, size_t cap, const uint8_t* s, size_t len) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 8 < cap; i++) {
    uint8_t c = s[i];
    if (c == '"' || c == '\\')      o += snprintf(out + o, cap - o, "\\%c", c);
    else if (c >= 0x20 && c < 0x7F) out[o++] = (char)c;
    else if (c == '\n')             o += snprintf(out + o, cap - o, "\\n");
    else if (c == '\t')             o += snprintf(out + o, cap - o, "\\t");
    else                            o += snprintf(out + o, cap - o, "\\u%04X", c);
  }
  return o;
}

// Decode one archive frame into a JSON object. Returns 0 if it is not a message
// frame (the mirror carries every push, not only messages).
static size_t frameToJson(const uint8_t* f, size_t flen, uint32_t seq, char* out, size_t cap) {
  if (flen < 2 || cap < 96) return 0;
  size_t o = 0;
  int8_t snr4 = (int8_t)f[1];

  if (f[0] == RESP_CONTACT_MSG && flen >= CONTACT_MSG_HDR) {
    char pk[PUBKEY_PREFIX_LEN * 2 + 1];
    mesh::Utils::toHex(pk, &f[4], PUBKEY_PREFIX_LEN);
    o += snprintf(out + o, cap - o,
                  "{\"seq\":%lu,\"kind\":\"contact\",\"from\":\"%s\",\"ts\":%lu,\"snr\":%.2f,\"text\":\"",
                  (unsigned long)seq, pk, (unsigned long)rdU32(&f[12]), snr4 / 4.0);
    o += jsonStr(out + o, cap - o, &f[CONTACT_MSG_HDR], flen - CONTACT_MSG_HDR);
    o += snprintf(out + o, cap - o, "\"}");
    return o;
  }
  if (f[0] == RESP_CHANNEL_MSG && flen >= CHANNEL_MSG_HDR) {
    o += snprintf(out + o, cap - o,
                  "{\"seq\":%lu,\"kind\":\"channel\",\"channel\":%u,\"ts\":%lu,\"snr\":%.2f,\"text\":\"",
                  (unsigned long)seq, (unsigned)f[4], (unsigned long)rdU32(&f[7]), snr4 / 4.0);
    o += jsonStr(out + o, cap - o, &f[CHANNEL_MSG_HDR], flen - CHANNEL_MSG_HDR);
    o += snprintf(out + o, cap - o, "\"}");
    return o;
  }
  return 0;
}

int botMessagesJson(int slot, const char* entity_id, uint32_t after, char* out, size_t cap) {
  uint32_t latest = entArchiveSeq(slot);
  size_t o = snprintf(out, cap, "{\"entity\":\"%s\",\"latest\":%lu,\"messages\":[",
                      entity_id, (unsigned long)latest);

  const size_t rawcap = 16 * 1024;
  uint8_t* raw = (uint8_t*)malloc(rawcap);
  if (raw == nullptr) { o += snprintf(out + o, cap - o, "]}"); return (int)o; }
  int n = entArchiveCopy(slot, after, raw, rawcap);

  bool first = true;
  int p = 0;
  while (p + 6 <= n) {
    uint32_t seq = rdU32(&raw[p]);
    uint16_t len = (uint16_t)raw[p + 4] | ((uint16_t)raw[p + 5] << 8);
    p += 6;
    if (p + len > n) break;
    char obj[MAX_FRAME_SIZE * 6 + 160];
    size_t used = frameToJson(&raw[p], len, seq, obj, sizeof(obj));
    p += len;
    if (used == 0) continue;                       // not a message frame
    if (o + used + 4 >= cap) break;                // caller's buffer is full
    if (!first) out[o++] = ',';
    memcpy(out + o, obj, used); o += used;
    first = false;
  }
  free(raw);
  o += snprintf(out + o, cap - o, "]}");
  return (int)o;
}

// ---- send ------------------------------------------------------------------

int botSend(int slot, const char* to_hex, const char* text, char* out, size_t cap) {
  if (to_hex == nullptr || text == nullptr || *text == 0) {
    snprintf(out, cap, "{\"error\":\"to and text are required\"}");
    return 400;
  }
  uint8_t pk[PUBKEY_PREFIX_LEN];
  if (strlen(to_hex) < PUBKEY_PREFIX_LEN * 2 ||
      !mesh::Utils::fromHex(pk, PUBKEY_PREFIX_LEN, to_hex)) {
    // fromHex wants exactly dest_size bytes of hex, so a longer prefix is
    // truncated to 6 bytes first — callers routinely hold a full 32-byte key.
    char trunc[PUBKEY_PREFIX_LEN * 2 + 1];
    strncpy(trunc, to_hex, PUBKEY_PREFIX_LEN * 2);
    trunc[PUBKEY_PREFIX_LEN * 2] = 0;
    if (!mesh::Utils::fromHex(pk, PUBKEY_PREFIX_LEN, trunc)) {
      snprintf(out, cap, "{\"error\":\"to must be at least %d hex chars\"}", PUBKEY_PREFIX_LEN * 2);
      return 400;
    }
  }

  size_t tlen = strlen(text);
  uint8_t frame[MAX_FRAME_SIZE];
  size_t need = 3 + 4 + PUBKEY_PREFIX_LEN + tlen;
  if (need > sizeof(frame)) {
    snprintf(out, cap, "{\"error\":\"text too long\"}");
    return 400;
  }
  size_t i = 0;
  frame[i++] = CMD_SEND_TXT;
  frame[i++] = 0;                 // txt type: plain
  frame[i++] = 0;                 // attempt
  uint32_t ts = rtc_clock.getCurrentTime();
  memcpy(&frame[i], &ts, 4); i += 4;
  memcpy(&frame[i], pk, PUBKEY_PREFIX_LEN); i += PUBKEY_PREFIX_LEN;
  memcpy(&frame[i], text, tlen); i += tlen;

  uint8_t resp[2048];
  int n = entFrameExchange(slot, frame, i, resp, sizeof(resp), 4000, 400);
  if (n <= 0) {
    snprintf(out, cap, "{\"error\":\"no response from entity\"}");
    return 503;
  }
  // response stream is [u16 len][frame]...
  int p = 0;
  while (p + 2 <= n) {
    uint16_t len = (uint16_t)resp[p] | ((uint16_t)resp[p + 1] << 8);
    p += 2;
    if (p + len > n || len < 1) break;
    const uint8_t* f = &resp[p];
    if (f[0] == RESP_SENT) {
      snprintf(out, cap, "{\"ok\":true,\"flood\":%s}", (len > 1 && f[1]) ? "true" : "false");
      return 200;
    }
    if (f[0] == RESP_ERR) {
      snprintf(out, cap, "{\"error\":\"send failed\",\"code\":%d}", len > 1 ? f[1] : -1);
      return 503;
    }
    p += len;
  }
  snprintf(out, cap, "{\"error\":\"unexpected response\"}");
  return 503;
}

// ---- webhook ---------------------------------------------------------------
// Push, so a bot does not have to poll. The hard constraint is that the mesh
// loop must never block on somebody else's web server: a webhook endpoint that
// hangs for 30 seconds would take the radio down with it. So the loop only ever
// enqueues, and a separate task owns the HTTP call.

#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static char           s_webhook[128] = {0};
static QueueHandle_t  s_wh_queue = nullptr;
static TaskHandle_t   s_wh_task = nullptr;
static volatile uint32_t s_wh_sent = 0, s_wh_failed = 0, s_wh_dropped = 0;
// Where each entity's archive had got to last time botTick looked. -1 slot is
// stored at index MULTI_MAX_CHAT_SLOTS.
static uint32_t s_seen_seq[MULTI_MAX_CHAT_SLOTS + 1] = {0};
static bool     s_seen_init = false;

#define WH_QUEUE_DEPTH 8

void botWebhookGet(char* out, size_t cap) { snprintf(out, cap, "%s", s_webhook); }
void botWebhookStats(uint32_t* sent, uint32_t* failed, uint32_t* dropped) {
  if (sent) *sent = s_wh_sent;
  if (failed) *failed = s_wh_failed;
  if (dropped) *dropped = s_wh_dropped;
}

void botWebhookSet(const char* url) {
  strncpy(s_webhook, url ? url : "", sizeof(s_webhook) - 1);
  s_webhook[sizeof(s_webhook) - 1] = 0;
  Preferences p;
  p.begin("multiwifi", false);
  p.putString("botwh", s_webhook);
  p.end();
}

static void webhookTask(void*) {
  for (;;) {
    char* payload = nullptr;
    if (xQueueReceive(s_wh_queue, &payload, portMAX_DELAY) != pdTRUE) continue;
    if (payload == nullptr) continue;
    if (s_webhook[0] == 0) { free(payload); continue; }

    esp_http_client_config_t cfg = {};
    cfg.url = s_webhook;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;
    // No certificate bundle is shipped, so https:// endpoints cannot be
    // verified. Rather than silently accept any certificate, this is documented
    // as an http:// (LAN) feature — see `set bot.webhook`.
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
      esp_http_client_set_header(c, "Content-Type", "application/json");
      esp_http_client_set_post_field(c, payload, strlen(payload));
      esp_err_t err = esp_http_client_perform(c);
      if (err == ESP_OK) s_wh_sent++; else s_wh_failed++;
      esp_http_client_cleanup(c);
    } else {
      s_wh_failed++;
    }
    free(payload);
  }
}

void botBegin() {
  Preferences p;
  p.begin("multiwifi", true);
  String u = p.getString("botwh", "");
  p.end();
  strncpy(s_webhook, u.c_str(), sizeof(s_webhook) - 1);
  s_webhook[sizeof(s_webhook) - 1] = 0;

  s_wh_queue = xQueueCreate(WH_QUEUE_DEPTH, sizeof(char*));
  if (s_wh_queue) {
    xTaskCreate(webhookTask, "botwh", 4096, nullptr, 3, &s_wh_task);
  }
}

// Queue one message payload. Never blocks: if the queue is full the message is
// DROPPED and counted, because the alternative is stalling the mesh loop behind
// a slow endpoint. The archive still holds it, so a bot that cannot miss
// anything polls /api/multi/bot/messages and treats the webhook as a wakeup.
static void webhookEnqueue(const char* entity, const char* msg_json, size_t len);
static void webhookEnqueue(const char* entity, const char* msg_json, size_t len) {
  if (s_webhook[0] == 0 || s_wh_queue == nullptr) return;
  size_t cap = len + 64;
  char* buf = (char*)malloc(cap);
  if (buf == nullptr) { s_wh_dropped++; return; }
  int n = snprintf(buf, cap, "{\"entity\":\"%s\",\"message\":%.*s}", entity, (int)len, msg_json);
  if (n <= 0) { free(buf); s_wh_dropped++; return; }
  if (xQueueSend(s_wh_queue, &buf, 0) != pdTRUE) { free(buf); s_wh_dropped++; }
}

bool botWebhookTest() {
  if (s_webhook[0] == 0 || s_wh_queue == nullptr) return false;
  uint32_t before = s_wh_dropped;
  const char* fake =
    "{\"seq\":0,\"kind\":\"contact\",\"from\":\"000000000000\",\"ts\":0,"
    "\"snr\":0.00,\"text\":\"webhook test from the node\"}";
  webhookEnqueue("test", fake, strlen(fake));
  return s_wh_dropped == before;
}

static void tickEntity(int slot, const char* id) {
  int idx = (slot < 0) ? MULTI_MAX_CHAT_SLOTS : slot;
  uint32_t latest = entArchiveSeq(slot);
  if (latest == s_seen_seq[idx]) return;
  // A reboot resets the sequence; don't replay the whole ring in that case.
  if (latest < s_seen_seq[idx]) { s_seen_seq[idx] = latest; return; }

  const size_t rawcap = 8 * 1024;
  uint8_t* raw = (uint8_t*)malloc(rawcap);
  if (raw == nullptr) return;
  int n = entArchiveCopy(slot, s_seen_seq[idx], raw, rawcap);
  s_seen_seq[idx] = latest;

  int p = 0;
  while (p + 6 <= n) {
    uint32_t seq = rdU32(&raw[p]);
    uint16_t len = (uint16_t)raw[p + 4] | ((uint16_t)raw[p + 5] << 8);
    p += 6;
    if (p + len > n) break;
    char obj[MAX_FRAME_SIZE * 6 + 160];
    size_t used = frameToJson(&raw[p], len, seq, obj, sizeof(obj));
    p += len;
    if (used) webhookEnqueue(id, obj, used);
  }
  free(raw);
}

void botTick() {
  // Initialise lazily, from the LOOP, rather than from setup(). Anything that
  // runs in setup() before network.begin() can cost remote access entirely if
  // it faults — there is no WiFi yet, so no OTA to recover with. Nothing here
  // is needed before the mesh is running, so it costs nothing to wait.
  static bool inited = false;
  if (!inited) { botBegin(); inited = true; }

  if (s_webhook[0] == 0) return;         // nothing to push to
  // First pass after boot: adopt the current sequence numbers without firing,
  // or enabling a webhook would replay the whole retained history at it.
  if (!s_seen_init) {
    s_seen_seq[MULTI_MAX_CHAT_SLOTS] = entArchiveSeq(-1);
    for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
      if (slotTypeGet(i) == SLOT_CHAT && multiChatSlotRunning(i)) s_seen_seq[i] = entArchiveSeq(i);
    }
    s_seen_init = true;
    return;
  }
  tickEntity(-1, "companion");
  for (int i = 0; i < MULTI_MAX_CHAT_SLOTS; i++) {
    if (slotTypeGet(i) != SLOT_CHAT || !multiChatSlotRunning(i)) continue;
    char id[8]; snprintf(id, sizeof(id), "chat%d", i + 1);
    tickEntity(i, id);
  }
}
