#pragma once

// A single "identity" (one stock example mesh) presented to the composition
// root as a small function table, so main.cpp never has to include the example
// headers directly (which would collide — three `struct NodePrefs`, three
// `class MyMesh`). Each wrapper TU renames its MyMesh via a macro, includes the
// stock example .cpp verbatim, and exposes exactly this interface.

#include <Arduino.h>
#include <FS.h>

typedef fs::FS MultiFS;   // == FILESYSTEM on ESP32

namespace mesh { class Radio; }

// Optional extra chat identities (see wrap_slots.cpp). 3 fixed roles + 5
// optional slots = 8 ports, matching SharedRadioCore::MAX_PORTS.
#define MULTI_MAX_CHAT_SLOTS 5

struct IdentityModule {
  const char* name;
  // construct the mesh on `port`, load/create identity from `fs` (its own
  // partition, so the stock "/identity" path is isolated), and begin().
  void (*setup)(MultiFS* fs, mesh::Radio* port);
  void (*loop)();
  // run a CLI command against this identity, reply into buf.
  void (*run_command)(const char* cmd, char* reply, size_t reply_size);
  void (*get_pubkey)(uint8_t out[32]);
  // Received packets this identity had to discard because its packet pool was
  // empty. The stock dispatcher only logs that in debug builds, so on a release
  // firmware it is a completely silent loss. Optional — may be null.
  uint32_t (*rx_pool_full)();
};
