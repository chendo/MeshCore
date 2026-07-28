#include "diag_service.h"
#include "multi_web.h"
#include <Preferences.h>
#include <helpers/SharedRadio.h>

static int8_t s_enabled = -1;   // -1 = not read from NVS yet

bool diagEnabled() {
  if (s_enabled < 0) {
    Preferences p;
    p.begin("multidiag", true);
    s_enabled = p.getBool("on", false) ? 1 : 0;
    p.end();
  }
  return s_enabled > 0;
}

void diagSetEnabled(bool on) {
  s_enabled = on ? 1 : 0;
  Preferences p;
  p.begin("multidiag", false);
  p.putBool("on", on);
  p.end();
}

static void fmtUtc(uint32_t epoch, char* out, size_t cap) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02dZ",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// Case-insensitive match of a leading command word, allowing an exact match or
// a following space so "!ping" and "!ping 3" both hit.
static bool isCmd(const char* text, const char* cmd) {
  size_t n = strlen(cmd);
  if (strncasecmp(text, cmd, n) != 0) return false;
  return text[n] == 0 || text[n] == ' ';
}

bool diagBuildReply(const char* text, mesh::Packet* pkt, mesh::RTCClock* rtc,
                    char* reply, size_t reply_cap) {
  if (text == nullptr || reply == nullptr || reply_cap < 32) return false;
  while (*text == ' ') text++;
  if (*text != '!') return false;

  uint8_t hops = pkt ? pkt->getPathHashCount() : 0;
  float snr = pkt ? pkt->getSNR() : 0.0f;

  if (isCmd(text, "!ping")) {
    // The request's own path and SNR describe the sender's link to us, which is
    // the thing they cannot measure from their end.
    if (hops == 0) {
      snprintf(reply, reply_cap, "pong: direct, no repeater. SNR %+.1f dB at me.", (double)snr);
    } else {
      snprintf(reply, reply_cap, "pong: %u hop%s to me, SNR %+.1f dB on the last one.",
               (unsigned)hops, hops == 1 ? "" : "s", (double)snr);
    }
    return true;
  }

  if (isCmd(text, "!time")) {
    uint32_t now = rtc ? rtc->getCurrentTime() : 0;
    char utc[24];
    fmtUtc(now, utc, sizeof(utc));
    uint32_t ago = multiClockSyncedAgo();
    // Staleness matters as much as the value: a clock nobody has checked in a
    // day means something different from one checked a minute ago, and both
    // read plausibly. Say so, and say plainly when it was never set at all.
    if (ago == 0) {
      snprintf(reply, reply_cap, "%s -- WARNING: clock never disciplined, treat as unreliable", utc);
    } else if (ago < 3600) {
      snprintf(reply, reply_cap, "%s, set from %s %lum ago", utc, multiClockSource(),
               (unsigned long)(ago / 60));
    } else {
      snprintf(reply, reply_cap, "%s, set from %s %luh %lum ago", utc, multiClockSource(),
               (unsigned long)(ago / 3600), (unsigned long)((ago % 3600) / 60));
    }
    return true;
  }

  if (isCmd(text, "!trace")) {
    // The hops the request actually traversed, newest last. Named where the
    // peer table has heard that node advert, hex prefix otherwise.
    if (pkt == nullptr || hops == 0) {
      snprintf(reply, reply_cap, "trace: you reached me directly, no repeaters in between.");
      return true;
    }
    uint8_t sz = pkt->getPathHashSize();
    size_t o = snprintf(reply, reply_cap, "trace (%u hop%s):", (unsigned)hops, hops == 1 ? "" : "s");
    SharedRadioCore* c = multiCore();
    for (uint8_t h = 0; h < hops && o < reply_cap - 12; h++) {
      const uint8_t* hop = &pkt->path[h * sz];
      const char* name = nullptr;
      if (c != nullptr) {
        for (int i = 0; i < c->numPeers(); i++) {
          const SharedRadioCore::PeerEntry* p = c->peer(i);
          if (p == nullptr || p->name[0] == 0) continue;
          uint8_t cmp = sz < p->width ? sz : p->width;
          if (memcmp(hop, p->hash, cmp) == 0) { name = p->name; break; }
        }
      }
      if (name) {
        o += snprintf(reply + o, reply_cap - o, " %s", name);
      } else {
        o += snprintf(reply + o, reply_cap - o, " %02x", hop[0]);
        if (sz > 1 && o < reply_cap - 4) o += snprintf(reply + o, reply_cap - o, "%02x", hop[1]);
      }
    }
    return true;
  }

  if (isCmd(text, "!help") || isCmd(text, "!")) {
    snprintf(reply, reply_cap,
             "!ping (hops+SNR to me) !time (my clock) !trace (your route here) !help");
    return true;
  }

  // An unrecognised "!" word is left alone rather than answered with an error:
  // it may simply be a message that happens to start with an exclamation mark,
  // and swallowing it would lose someone's text.
  return false;
}
