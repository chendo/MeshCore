#pragma once
// Minimal Arduino surface for native tests of the multi-identity components.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

extern unsigned long g_fake_millis;
inline unsigned long millis() { return g_fake_millis; }
inline void delay(unsigned long ms) { g_fake_millis += ms; }
inline void* ps_malloc(size_t n) { return malloc(n); }

struct FakeSerial {
  template <typename... A> void printf(const char*, A...) {}
  void println(const char* = "") {}
  void print(const char*) {}
};
extern FakeSerial Serial;
