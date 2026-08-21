#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// A real SHA-256 and HMAC-SHA256 for the host tests, with the same interface
// that rweather/Crypto gives the firmware.
//
// The stand-in in test/mocks is deliberately not cryptographic, and its
// finalizeHMAC() writes nothing at all. That is fine where a hash only has to
// separate one packet from another. It is not fine here: the whole point of the
// BLE bridge frame is that a wrong key produces a different tag, so a test
// against a no-op HMAC would pass whatever the code did.
class SHA256 {
public:
  SHA256() { reset(); }

  void reset() {
    _h[0] = 0x6a09e667; _h[1] = 0xbb67ae85; _h[2] = 0x3c6ef372; _h[3] = 0xa54ff53a;
    _h[4] = 0x510e527f; _h[5] = 0x9b05688c; _h[6] = 0x1f83d9ab; _h[7] = 0x5be0cd19;
    _len = 0; _buf_len = 0;
  }

  void update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    _len += len;
    while (len > 0) {
      size_t take = 64 - _buf_len;
      if (take > len) take = len;
      memcpy(_buf + _buf_len, p, take);
      _buf_len += take; p += take; len -= take;
      if (_buf_len == 64) { block(_buf); _buf_len = 0; }
    }
  }

  void finalize(uint8_t* hash, size_t hashLen) {
    uint32_t h[8];
    uint8_t full[32];
    memcpy(h, _h, sizeof(h));
    // Pad a copy, so the object stays usable if a caller finalizes twice.
    uint8_t buf[64];
    size_t buf_len = _buf_len;
    uint64_t bits = (uint64_t)_len * 8;
    memcpy(buf, _buf, buf_len);
    buf[buf_len++] = 0x80;
    if (buf_len > 56) {
      memset(buf + buf_len, 0, 64 - buf_len);
      blockWith(h, buf);
      buf_len = 0;
    }
    memset(buf + buf_len, 0, 56 - buf_len);
    for (int i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    blockWith(h, buf);
    for (int i = 0; i < 8; i++) {
      full[4 * i + 0] = (uint8_t)(h[i] >> 24);
      full[4 * i + 1] = (uint8_t)(h[i] >> 16);
      full[4 * i + 2] = (uint8_t)(h[i] >> 8);
      full[4 * i + 3] = (uint8_t)(h[i]);
    }
    if (hashLen > 32) hashLen = 32;
    memcpy(hash, full, hashLen);
  }

  void resetHMAC(const void* key, size_t keyLen) {
    uint8_t pad[64];
    keyPad(key, keyLen, pad, 0x36);
    reset();
    update(pad, 64);
  }

  void finalizeHMAC(const void* key, size_t keyLen, uint8_t* hash, size_t hashLen) {
    uint8_t inner[32];
    finalize(inner, 32);
    uint8_t pad[64];
    keyPad(key, keyLen, pad, 0x5c);
    reset();
    update(pad, 64);
    update(inner, 32);
    finalize(hash, hashLen);
  }

private:
  void keyPad(const void* key, size_t keyLen, uint8_t pad[64], uint8_t xorByte) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (keyLen > 64) {
      SHA256 kh;
      kh.update(key, keyLen);
      kh.finalize(k, 32);
    } else {
      memcpy(k, key, keyLen);
    }
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ xorByte);
  }

  static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t* p) { blockWith(_h, p); }

  static void blockWith(uint32_t* h, const uint8_t* p) {
    static const uint32_t K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16)
           | ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  uint32_t _h[8];
  uint8_t _buf[64];
  size_t _buf_len;
  uint64_t _len;
};
