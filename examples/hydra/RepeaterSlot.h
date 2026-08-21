#pragma once

// Slot 0: the stock simple_repeater identity, unmodified.
//
// MyMesh already takes a mesh::Radio& — it has no idea whether that is the
// board's transceiver or one port of a shared one. So reuse here is literally
// just handing it a RadioPort instead of radio_driver; no fork of the repeater,
// no #define rename of its class, no edits in examples/simple_repeater.
// The build pulls in that directory's MyMesh.cpp and nothing else (its main.cpp
// carries its own setup()/loop() and is excluded).

#include "HydraSlot.h"
#include <MyMesh.h>          // examples/simple_repeater, on the include path
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>

class RepeaterSlot : public HydraSlot {
  RadioPort        _port;
  ArduinoMillis    _ms;
  StdRNG           _rng;
  SimpleMeshTables _tables;   // full 160-hash dedup table; the repeater sees everything
  MyMesh           _mesh;
  bool             _begun;

public:
  RepeaterSlot()
      : _mesh(board, _port, _ms, _rng, rtc_clock, _tables), _begun(false) {}

  RadioPort& port() override { return _port; }
  SlotType type() const override { return SLOT_REPEATER; }
  const mesh::LocalIdentity& identity() const override { return _mesh.self_id; }
  // Slot 0's name lives in prefs.json, owned by CommonCLI, not in the hydra
  // slot config: it is the stock repeater identity and `set name` still works
  // on it unqualified. ADVERT_NAME means it is never unnamed.
  const char* name() const override { return const_cast<MyMesh&>(_mesh).getNodePrefs()->node_name; }
  bool hasPendingWork() const override { return _begun && _mesh.hasPendingWork(); }
  MyMesh& mesh() { return _mesh; }

  bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name,
             const char* display_name, SlotType type) override {
    if (!store.load(id_name, _mesh.self_id)) {
      _mesh.self_id = radio_new_identity();
      // 0x00 and 0xFF are reserved id-hash prefixes
      for (int i = 0; i < 10 && (_mesh.self_id.pub_key[0] == 0x00 || _mesh.self_id.pub_key[0] == 0xFF); i++) {
        _mesh.self_id = radio_new_identity();
      }
      store.save(id_name, _mesh.self_id);
    }
    _mesh.begin(fs);   // also pushes freq/bw/sf/cr/power onto the real radio
    _begun = true;
    return true;
  }

  void loop() override { if (_begun) _mesh.loop(); }

  void handleCommand(uint32_t sender_timestamp, char* command,
                     char* reply, size_t reply_sz) override {
    // reply must be >= 160 bytes: MyMesh's CLI writes into it unbounded.
    if (_begun) _mesh.handleCommand(sender_timestamp, command, reply);
  }

  NodePrefs* prefs() { return _mesh.getNodePrefs(); }
};
