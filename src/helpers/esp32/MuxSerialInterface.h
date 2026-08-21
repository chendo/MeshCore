#pragma once

#include <Arduino.h>
#include <helpers/esp32/SerialWifiInterface.h>

// This class lets a second local client drive the app protocol of a companion
// identity. That client can be a web UI or a bot. It uses the SAME serial
// interface as the phone app. The client can also read the message history
// without a change to the offline queue of the device. Use one instance for
// each chat identity.
//
// Routing rules. The mesh loop is single-threaded. The code writes the
// responses to a command while it processes that command.
//   * checkRecvFrame() takes a web frame first if one is pending. If it takes
//     a web frame, the web becomes the owner of the next responses. If it takes
//     a TCP frame, the app becomes the owner.
//   * writeFrame(): async push codes (>= 0x80) always go to the TCP socket of
//     the app. Response codes (< 0x80) go to the side that sent the command
//     that the code processes now.
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

  // BaseSerialInterface. The mesh loop task calls these.
  void enable() override { tcp.enable(); }
  void disable() override { tcp.disable(); }
  bool isEnabled() const override { return tcp.isEnabled(); }
  // Report "connected" while the web UI also exchanges frames. The stock code
  // gates some async results on isConnected(), for example the trace data. The
  // web needs a copy of those results even when no phone is attached.
  bool isConnected() const override {
    // A _last_web_ms of 0 means that the web has never driven this identity.
    // Without that guard, the mux reports a connection for the first minute
    // after the boot, while millis() is still near zero.
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
    // Copy every message frame that passes through into the archive, whichever
    // side synced it. The web UI can then show the history without a change to
    // the offline queue of the companion. The code also copies the trace
    // results (0x89), so the web traceroute can read them.
    // The archive holds the messages, plus EVERY async push (0x80..0x8F: login
    // result, path update, send confirmation, trace, new advert and so on).
    // The code writes the pushes only to the TCP socket of the phone. Without
    // this copy, the web UI can never see the result of anything that it
    // starts, for example a room login.
    if (len > 0 && (src[0] == 7 || src[0] == 8 || src[0] == 16 || src[0] == 17 || src[0] == 27 ||
                    (src[0] >= 0x80 && src[0] <= 0x8F))) {
      archiveAdd(src, len);
    }
    if (len > 0 && src[0] >= 0x80) {   // async push goes to the phone app
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

  // The web side. The httpd task calls these, and it holds the exchange mutex.
  bool webStart(const uint8_t* frame, size_t len) {
    if (_in_state != 0 || len == 0 || len > sizeof(_in_buf)) return false;
    memcpy(_in_buf, frame, len);
    _in_len = len;
    _resp_len = 0;
    _last_resp_ms = millis();
    _last_web_ms = millis();
    _in_state = 1;   // write this last: it gives the frame to the mesh loop
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

  // The message archive. It holds a copy of the synced message frames. The web
  // can only read it.
  uint32_t archiveSeq() const { return _arch_seq; }
  // Copy the entries with a seq greater than `after` into out. The format is
  // [u32 seq][u16 len][frame]...
  int archiveCopy(uint32_t after, uint8_t* out, size_t cap) {
    if (_arch == nullptr) return 0;
    uint32_t newest = _arch_seq;
    uint32_t oldest = newest > ARCH_SLOTS ? newest - ARCH_SLOTS : 0;
    if (after < oldest) after = oldest;
    size_t o = 0;
    // The loop task continues to write while this code runs, because the web
    // reads the archive from the HTTPS task. Therefore the code can reuse a
    // slot during the copy. The code writes the sequence number last, and it
    // checks that number again AFTER the copy as well as before. A slot that
    // turns over during the memcpy gives a frame that is part old and part
    // new, and the panel would show that frame as a corrupt message. To
    // discard it is correct. The caller polls again and reads it as a newer
    // entry.
    for (uint32_t s = after + 1; s <= newest; s++) {
      ArchSlot& a = _arch[(s - 1) % ARCH_SLOTS];
      if (a.seq != s) continue;
      uint16_t len = a.len;
      if (len > MAX_FRAME_SIZE) continue;
      if (o + 6 + len > cap) break;
      memcpy(out + o, &s, 4);
      out[o + 4] = len & 0xFF; out[o + 5] = (len >> 8) & 0xFF;
      memcpy(out + o + 6, a.buf, len);
      if (a.seq != s) continue;               // the slot was reused during the copy
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
    a.seq = _arch_seq + 1;   // the code writes this last
    _arch_seq = _arch_seq + 1;
  }

  volatile uint8_t _in_state = 0;   // 0 idle, 1 frame pending, 2 taken (the code collects responses)
  uint8_t  _in_buf[MAX_FRAME_SIZE];
  volatile size_t _in_len = 0;
  uint8_t* _resp = nullptr;
  size_t   _resp_cap = 0;
  volatile size_t _resp_len = 0;
  volatile uint32_t _last_resp_ms = 0;
  volatile uint32_t _last_web_ms = 0;
  volatile bool _route_web = false;
};

