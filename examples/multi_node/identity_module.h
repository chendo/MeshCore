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

struct IdentityModule {
  const char* name;
  // construct the mesh on `port`, load/create identity from `fs` (its own
  // partition, so the stock "/identity" path is isolated), and begin().
  void (*setup)(MultiFS* fs, mesh::Radio* port);
  void (*loop)();
  // run a CLI command against this identity, reply into buf.
  void (*run_command)(const char* cmd, char* reply, size_t reply_size);
  void (*get_pubkey)(uint8_t out[32]);
};
