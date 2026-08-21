#pragma once

#include <Arduino.h>
#include <helpers/esp32/SerialWifiInterface.h>

// Lets a second, local client (a web UI, a bot) drive a companion identity's
// app protocol over the SAME serial interface the phone app is using, and read
// message history without consuming the device's offline queue. One per chat
// identity.
//
// Routing rules (single-threaded mesh loop; responses to a command are written
// synchronously while that command is being handled):
//   * checkRecvFrame() prefers a pending web frame; consuming one makes the web
//     the owner of subsequent responses, consuming a TCP frame makes the app.
//   * writeFrame(): async push codes (>= 0x80) always go to the app's TCP
//     socket; response codes (< 0x80) go to whichever side issued the command
//     being processed.
class MuxSerialInterface : public BaseSerialInterface {
public:
  SerialWifiInterface tcp;

  void init(size_t resp_cap) {
    _resp = (uint8_t*)ps_malloc(resp_cap);
    if (_resp == nullptr) { resp_cap = 4096; _resp = (uint8_t*)malloc(resp_cap); }
    _resp_cap = _resp ? resp_cap : 0;
    _arch = (ArchSlot*)ps_malloc(sizeof(ArchSlot) * ARCH_SLOTS);
    if (_arch != nullptr) memset(_arch, 0, sizeof(ArchSlot) * ARCH_SLOTS);
  }

  // BaseSerialInterface (called from the mesh loop task)
  void enable() override { tcp.enable(); }
  void disable() override { tcp.disable(); }
  bool isEnabled() const override { return tcp.isEnabled(); }
  // Report "connected" while the web UI is actively exchanging frames too:
  // stock code gates some async results (e.g. trace data) on isConnected(),
  // and the web needs those mirrored even with no phone attached.
  bool isConnected() const override {
    // _last_web_ms == 0 means the web has never driven this identity; without
    // that guard the mux claims to be connected for the first minute after
    // boot, when millis() is still close to zero.
    return tcp.isConnected() || (_last_web_ms != 0 && millis() - _last_web_ms < 60000);
  }
  bool isWriteBusy() const override { return tcp.isWriteBusy(); }

  size_t checkRecvFrame(uint8_t dest[]) override {
    if (_in_state == 1) {
      size_t n = _in_len;
      memcpy(dest, _in_buf, n);
      _route_web = true;
      _last_resp_ms = millis();
      _in_state = 2;
      return n;
    }
    size_t n = tcp.checkRecvFrame(dest);
    if (n > 0) _route_web = false;
    return n;
  }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    // Mirror every message frame passing through (whichever side synced it)
    // into the archive, so the web UI can show history without consuming
    // anything from the companion's offline queue. Trace results (0x89) are
    // mirrored too so the web traceroute can pick them up.
    // messages, plus EVERY async push (0x80..0x8F: login result, path update,
    // send confirmation, trace, new advert...). Pushes are written only to the
    // phone's TCP socket, so without mirroring the web UI can never observe
    // the outcome of anything it initiates (e.g. a room login).
    if (len > 0 && (src[0] == 7 || src[0] == 8 || src[0] == 16 || src[0] == 17 || src[0] == 27 ||
                    (src[0] >= 0x80 && src[0] <= 0x8F))) {
      archiveAdd(src, len);
    }
    if (len > 0 && src[0] >= 0x80) {   // async push -> phone app
      return tcp.writeFrame(src, len);
    }
    if (_route_web && _in_state == 2 && _resp != nullptr) {
      if (_resp_len + 2 + len <= _resp_cap) {
        _resp[_resp_len] = len & 0xFF;
        _resp[_resp_len + 1] = (len >> 8) & 0xFF;
        memcpy(_resp + _resp_len + 2, src, len);
        _resp_len = _resp_len + 2 + len;
        _last_resp_ms = millis();
      }
      return len;
    }
    return tcp.writeFrame(src, len);
  }

  // web side (called from the httpd task; caller holds the exchange mutex)
  bool webStart(const uint8_t* frame, size_t len) {
    if (_in_state != 0 || len == 0 || len > sizeof(_in_buf)) return false;
    memcpy(_in_buf, frame, len);
    _in_len = len;
    _resp_len = 0;
    _last_resp_ms = millis();
    _last_web_ms = millis();
    _in_state = 1;   // set last: publishes the frame to the mesh loop
    return true;
  }
  size_t webRespLen() const { return _resp_len; }
  uint32_t webMsSinceLastResp() const { return millis() - _last_resp_ms; }
  int webFinish(uint8_t* out, size_t out_cap) {
    size_t n = _resp_len;
    if (n > out_cap) n = out_cap;
    if (n > 0) memcpy(out, _resp, n);
    _route_web = false;
    _resp_len = 0;
    _in_state = 0;
    return (int)n;
  }

  // message archive (mirror of synced message frames; read-only for the web)
  uint32_t archiveSeq() const { return _arch_seq; }
  // copy entries with seq > after into out as [u32 seq][u16 len][frame]...
  int archiveCopy(uint32_t after, uint8_t* out, size_t cap) {
    if (_arch == nullptr) return 0;
    uint32_t newest = _arch_seq;
    uint32_t oldest = newest > ARCH_SLOTS ? newest - ARCH_SLOTS : 0;
    if (after < oldest) after = oldest;
    size_t o = 0;
    // The loop task keeps writing while this runs (the web reads it from the
    // HTTPS task), so a slot can be recycled underneath us. The sequence number
    // is written last, and re-checked AFTER the copy as well as before: a slot
    // that turned over mid-memcpy yields a half-old/half-new frame, which the
    // panel would render as a corrupt message. Dropping it is correct — the
    // caller polls again and picks it up as a newer entry.
    for (uint32_t s = after + 1; s <= newest; s++) {
      ArchSlot& a = _arch[(s - 1) % ARCH_SLOTS];
      if (a.seq != s) continue;
      uint16_t len = a.len;
      if (len > MAX_FRAME_SIZE) continue;
      if (o + 6 + len > cap) break;
      memcpy(out + o, &s, 4);
      out[o + 4] = len & 0xFF; out[o + 5] = (len >> 8) & 0xFF;
      memcpy(out + o + 6, a.buf, len);
      if (a.seq != s) continue;               // recycled while we copied
      o += 6 + len;
    }
    return (int)o;
  }

private:
  static const int ARCH_SLOTS = 128;
  struct ArchSlot { uint32_t seq; uint16_t len; uint8_t buf[MAX_FRAME_SIZE]; };
  ArchSlot* _arch = nullptr;
  volatile uint32_t _arch_seq = 0;

  void archiveAdd(const uint8_t* src, size_t len) {
    if (_arch == nullptr || len > MAX_FRAME_SIZE) return;
    ArchSlot& a = _arch[_arch_seq % ARCH_SLOTS];
    a.len = (uint16_t)len;
    memcpy(a.buf, src, len);
    a.seq = _arch_seq + 1;   // written last
    _arch_seq = _arch_seq + 1;
  }

  volatile uint8_t _in_state = 0;   // 0 idle, 1 frame pending, 2 consumed (collecting responses)
  uint8_t  _in_buf[MAX_FRAME_SIZE];
  volatile size_t _in_len = 0;
  uint8_t* _resp = nullptr;
  size_t   _resp_cap = 0;
  volatile size_t _resp_len = 0;
  volatile uint32_t _last_resp_ms = 0;
  volatile uint32_t _last_web_ms = 0;
  volatile bool _route_web = false;
};

