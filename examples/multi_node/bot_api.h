#pragma once

// A bot-shaped API over the chat identities.
//
// The panel already talks to a chat identity through /api/multi/comp/frame and
// /api/multi/comp/archive, but those carry the raw MeshCore companion protocol:
// binary frames, hand-packed offsets, response codes. That is the right
// interface for a client that reimplements the protocol, and the wrong one for
// somebody writing a bot in twenty lines of Python.
//
// This exposes the same two capabilities as JSON, and adds the thing polling
// cannot give you — a webhook, so a message arriving on the mesh reaches an
// external service without anyone asking.
//
// EVERY entry point takes an ENTITY. The node runs several chat identities (the
// fixed companion plus any slot configured as type=chat, see slot_types.h), each
// with its own keypair, contacts and message queue. A bot addresses one of them
// by name, so it can have an identity of its own rather than sharing the
// operator's.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Entity ids: "companion", or "chat1".."chatN" for a slot running as chat.
// Returns true and fills `slot` (-1 = the fixed companion, 0-based slot index
// otherwise) when the id names an identity that is currently RUNNING.
bool botResolveEntity(const char* id, int* slot);

// JSON list of the entities a bot may currently address, e.g.
//   {"entities":[{"id":"companion","running":true},{"id":"chat2",...}]}
int botEntitiesJson(char* out, size_t cap);

// Decoded messages newer than `after`, as JSON:
//   {"latest":41,"entity":"companion","messages":[
//     {"seq":40,"kind":"contact","from":"a1b2c3d4e5f6","ts":1786...,"snr":-3.5,"text":"hi"}]}
// `kind` is "contact" (a DM) or "channel" (a group message, with "channel":N).
// Non-message frames in the mirror are skipped rather than reported.
int botMessagesJson(int slot, const char* entity_id, uint32_t after, char* out, size_t cap);

// Send a text message. `to_hex` is a contact's public-key prefix (>= 12 hex
// chars / 6 bytes, as reported in "from"). Writes a JSON result into out.
// Returns an HTTP-ish status: 200 ok, 400 bad request, 503 entity unavailable.
int botSend(int slot, const char* to_hex, const char* text, char* out, size_t cap);

// ---- webhook ---------------------------------------------------------------
// When set, every newly-arrived message on any running chat entity is POSTed as
// JSON to this URL. Delivery is attempted once from a background task and is
// explicitly best-effort: the mesh loop must never wait on somebody else's HTTP
// server, so a slow or dead endpoint drops messages rather than stalling the
// node. The archive remains the reliable source — a bot that must not miss
// anything polls it and treats the webhook purely as a wakeup.
void botWebhookGet(char* out, size_t cap);
void botWebhookSet(const char* url);
void botWebhookStats(uint32_t* sent, uint32_t* failed, uint32_t* dropped);
// Enqueue a synthetic message so an endpoint can be tested without waiting for
// somebody to actually send a DM over the mesh. Returns false if no webhook is
// configured or the queue is full.
bool botWebhookTest();

// Call from the main loop: notices new archive entries and queues them.
void botTick();
// Call once at startup, after WiFi config is loaded.
void botBegin();
