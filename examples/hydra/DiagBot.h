#pragma once

// A diagnostic bot that answers text commands over the mesh, so that any
// MeshCore client can ask a node about the link and read the answer as an
// ordinary message. It answers `ping`, `trace` and `peers`. The verbs and every
// reply string live in DiagFormat.h, which a host test covers.
//
// DIRECT MESSAGES ONLY (decision 12). The bot hangs off onMessageRecv(), which
// is the plain 1:1 text path. It never answers a channel message and it never
// answers a command that arrives for somebody else. Two things follow. One
// packet can never make many nodes transmit, because the sender must hold our
// shared secret. And a return path always exists, because the sender is a
// contact.
//
// OFF BY DEFAULT, one setting for each slot: `slot N set diag on`. A node that
// nobody configured answers nothing.
//
// THE BOT REPLIES ONLY WHEN THE RATE LIMIT LETS IT. A refusal message costs the
// same airtime as the answer, so a refusal defeats the limit that produced it.
// Silence is the refusal. The refusals that DO get a reply are the ones that
// describe a state, such as "I have no route to you yet", because the requester
// can act on those and they sit inside the budget that the limiter already
// granted.

#include "DiagFormat.h"
#include <helpers/BaseChatMesh.h>
#include <helpers/ClientACL.h>
#include <helpers/MeshObserver.h>

// The peers reply goes out as this many messages at most: one summary and the
// rest of rows. The console `peers` command prints the whole table. This is a
// radio link, so the bot sends the head of the table and says how much it left.
#ifndef DIAG_PEERS_MAX_MSGS
  #define DIAG_PEERS_MAX_MSGS  3
#endif

// How long a trace stays outstanding. It is shorter than the 300 s burst window
// of a trace, so the next trace that the limiter allows always finds the slot
// free. That is why this class needs no loop hook.
#define DIAG_TRACE_TIMEOUT_MS  120000

class DiagBot {
public:
  DiagBot()
      : _radio(nullptr), _obs(nullptr), _acl(nullptr), _configured_cr(nullptr),
        _trace_tag(0), _trace_auth(0), _trace_until(0), _trace_relays(0),
        _enabled(false), _trace_busy(false) {
    memset(_trace_to, 0, sizeof(_trace_to));
  }

  // The node supplies these, because none of them belong to an identity. The
  // radio and the observer are node-scoped, and slot 0 owns the radio settings
  // (decision 8). The pointer to the coding rate stays live, so `set cr` on the
  // node changes what the ping reply says without any further wiring.
  void attach(mesh::Radio* radio, const MeshObserver* obs, const uint8_t* configured_cr) {
    _radio = radio; _obs = obs; _configured_cr = configured_cr;
  }

  // The gate for `peers`. A null ACL means that nobody can be an admin here,
  // which is the correct answer for a chat slot: it has no ACL at all.
  void setAuth(ClientACL* acl) { _acl = acl; }

  bool enabled() const { return _enabled; }
  void setEnabled(bool on) { _enabled = on; }

  // ------------------------------------------------------ the receive hook

  void onText(BaseChatMesh& m, const ContactInfo& from, mesh::Packet* pkt,
              uint32_t sender_timestamp, const char* text) {
    if (!_enabled) return;
    uint32_t now = m.getRTCClock()->getCurrentTime();
    char reply[MAX_TEXT_LEN + 1];

    switch (diagParseCommand(text)) {
      case DIAG_PING:
        if (!_limits.allowPing(now)) return;
        formatPing(m, pkt, reply, sizeof(reply));
        send(m, from, reply);
        return;

      case DIAG_TRACE: {
        if (!_limits.allowTrace(now)) return;
        DiagTraceRefusal r = startTrace(m, from);
        if (r != DIAG_TRACE_OK) send(m, from, diagTraceRefusalText(r));
        return;
      }

      case DIAG_PEERS:
        if (!_limits.allowOther(now)) return;
        if (!isAdmin(from)) { send(m, from, diagPeersDeniedText()); return; }
        sendPeers(m, from);
        return;

      default:
        if (!_limits.allowOther(now)) return;
        send(m, from, diagUsageText());
        return;
    }
  }

  // The result of a trace that WE started comes back here, on this node. The
  // client took no part beyond the message that asked for it.
  void onTraceResult(BaseChatMesh& m, mesh::Packet* pkt, uint32_t tag, uint32_t auth_code,
                     uint8_t flags, const uint8_t* path_snrs, const uint8_t* path_hashes,
                     uint8_t hash_bytes) {
    // Check the auth code before anything else. A trace result is an unsigned
    // packet that any node can transmit at us. The tag and the auth code are
    // random and are alive only while our own trace is outstanding, so a forged
    // result cannot make us transmit and cannot make us report a route that
    // nobody took.
    if (!_trace_busy || !_enabled) return;
    if (m.millisHasNowPassed(_trace_until)) { _trace_busy = false; return; }
    if (tag != _trace_tag || auth_code != _trace_auth) return;

    ContactInfo* to = m.lookupContactByPubKey(_trace_to, PUB_KEY_SIZE);
    _trace_busy = false;                      // one result for one request
    if (to == NULL) return;                   // the requester is gone from the contacts

    uint8_t entry_size = 1 << (flags & 0x03);
    uint8_t hops = entry_size ? (uint8_t)(hash_bytes / entry_size) : 0;
    char reply[MAX_TEXT_LEN + 1];
    diagFormatTrace(reply, sizeof(reply), path_hashes, entry_size, (const int8_t*)path_snrs,
                    hops, (int8_t)(pkt->getSNR() * 4), _trace_relays);
    send(m, *to, reply);
  }

private:
  void send(BaseChatMesh& m, const ContactInfo& to, const char* text) {
    uint32_t expected_ack, est_timeout;
    m.sendMessage(to, m.getRTCClock()->getCurrentTimeUnique(), 0, text, expected_ack, est_timeout);
  }

  bool isAdmin(const ContactInfo& from) const {
    if (_acl == nullptr) return false;   // a chat slot carries no ACL at all
    ClientInfo* c = _acl->getClient(from.id.pub_key, PUB_KEY_SIZE);
    return diagPeersAllowed(c ? &c->permissions : nullptr);
  }

  void formatPing(BaseChatMesh& m, mesh::Packet* pkt, char* out, size_t cap) {
    uint8_t hash[MAX_HASH_SIZE];
    pkt->calculatePacketHash(hash);

    DiagPingFacts f;
    f.route_flood = pkt->isRouteFlood();
    // A direct packet reaches its destination with an empty path, because every
    // relay took itself out of it. So the count is meaningful for a flood only.
    f.hops = f.route_flood ? pkt->getPathHashCount() : 0;
    f.hash_size = pkt->getPathHashSize();
    f.path = pkt->path;
    f.snr4 = pkt->_snr;
    f.rssi = pkt->_rssi;
    f.cr_observed = pkt->getCodingRate() != 0;
    f.cr = f.cr_observed ? pkt->getCodingRate()
                         : (_configured_cr ? *_configured_cr : 0);
    // getEstAirtimeForCR() prices the frame at the rate of the SENDER when the
    // radio reported one, and at our own rate when it did not. The label above
    // says which of the two happened.
    f.airtime_ms = _radio ? _radio->getEstAirtimeForCR(pkt->getRawLength(), pkt->getCodingRate()) : 0;
    memcpy(f.hash, hash, 4);
    diagFormatPing(out, cap, f);
  }

  DiagTraceRefusal startTrace(BaseChatMesh& m, const ContactInfo& from) {
    if (_trace_busy && !m.millisHasNowPassed(_trace_until)) return DIAG_TRACE_IN_FLIGHT;
    if (from.out_path_len == OUT_PATH_UNKNOWN) return DIAG_TRACE_NO_PATH;

    uint8_t entry_size = (uint8_t)((from.out_path_len >> 6) + 1);
    uint8_t relays = (uint8_t)(from.out_path_len & 63);
    if (relays == 0) return DIAG_TRACE_ZERO_HOP;

    // The trace must follow the route at the width of the route. The two
    // encodings do not agree on every width, so ask before you build.
    uint8_t flags = diagTraceFlagsForWidth(entry_size);
    if (flags == 0xFF) return DIAG_TRACE_BAD_WIDTH;

    uint8_t path[MAX_PACKET_PAYLOAD];
    uint8_t len = diagBuildTracePath(path, sizeof(path), from.out_path, relays, entry_size);
    if (len == 0) return DIAG_TRACE_TOO_LONG;

    uint32_t tag, auth;
    m.getRNG()->random((uint8_t*)&tag, 4);
    m.getRNG()->random((uint8_t*)&auth, 4);
    mesh::Packet* pkt = m.createTrace(tag, auth, flags);
    if (pkt == NULL) return DIAG_TRACE_NO_PACKET;

    m.sendDirect(pkt, path, len);
    _trace_tag = tag;
    _trace_auth = auth;
    _trace_relays = relays;
    _trace_until = m.futureMillis(DIAG_TRACE_TIMEOUT_MS);
    _trace_busy = true;
    memcpy(_trace_to, from.id.pub_key, PUB_KEY_SIZE);
    // No reply goes out here. The result itself is the answer, and a second
    // message doubles what a trace costs this node.
    return DIAG_TRACE_OK;
  }

  void sendPeers(BaseChatMesh& m, const ContactInfo& to) {
    if (_obs == nullptr) return;
    char msg[MAX_TEXT_LEN + 1];
    uint32_t now = millis();
    int total = _obs->numPeers();

    // Message 1 of the budget is the summary. It carries the hash widths and
    // the counts, which say how strong the picture is.
    formatPeerSummary(msg, sizeof(msg), *_obs);
    send(m, to, msg);

    // The remaining budget holds rows, and the last message of the budget says
    // what the bot left out.
    int shown = 0;
    for (int msgs = 2; msgs < DIAG_PEERS_MAX_MSGS && shown < total; msgs++) {
      int n = 0;
      while (shown < total) {
        char line[80];
        int w = diagFormatPeerLine(line, sizeof(line), *_obs->peer(shown), now);
        if (w < 0 || n + w + 1 >= (int)sizeof(msg)) break;
        n += snprintf(msg + n, sizeof(msg) - n, "%s%s", n ? "\n" : "", line);
        shown++;
      }
      if (n == 0) break;
      send(m, to, msg);
    }
    snprintf(msg, sizeof(msg), "peers: %d of %d rows sent. The console lists them all.",
             shown, total);
    send(m, to, msg);
  }

  mesh::Radio*         _radio;
  const MeshObserver*  _obs;
  ClientACL*           _acl;
  const uint8_t*       _configured_cr;
  DiagLimits           _limits;
  uint32_t             _trace_tag, _trace_auth;
  unsigned long        _trace_until;
  uint8_t              _trace_to[PUB_KEY_SIZE];
  uint8_t              _trace_relays;
  bool                 _enabled;
  bool                 _trace_busy;
};
