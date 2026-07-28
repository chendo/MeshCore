#pragma once

// Public diagnostics responder for a chat identity.
//
// A chat identity normally queues every incoming direct message for a phone app
// to collect. With no app attached those just pile up, so a slot running
// headless is dead weight. This turns one into something useful to the mesh: it
// answers a handful of "!" commands directly, so any user can measure their
// link to this node without needing an account, a password, or an app feature.
//
// Deliberately a DIRECT MESSAGE service rather than a room. A room post is
// re-encrypted and re-sent to every member individually (see
// simple_room_server pushPostToClient) — one query would cost N transmissions
// plus ACKs, and flood for any member whose path is unknown. Here it is one
// request in, one reply out.
//
// Everything answered is read-only and already public: hop count and SNR are
// properties of the packet the sender just transmitted, and the clock is
// already exposed unauthenticated via ANON_REQ_TYPE_BASIC. So there is nothing
// to gate, which is what makes it usable as an open service.

#include <Arduino.h>
#include <Mesh.h>

// Longest reply we will compose. Kept well inside one packet so a diagnostic
// answer never fragments or gets dropped for size on a marginal link.
#define DIAG_REPLY_MAX 132

// Returns true and fills `reply` if `text` is a diagnostics command.
// `pkt` is the message that carried the request — its path and SNR ARE the
// measurement, which is why this has to run where the packet is still in hand.
bool diagBuildReply(const char* text, mesh::Packet* pkt, mesh::RTCClock* rtc,
                    char* reply, size_t reply_cap);

// Whether the service is switched on (persisted; see `set diag on|off`).
bool diagEnabled();
void diagSetEnabled(bool on);
