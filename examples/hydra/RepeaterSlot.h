#pragma once

// Slot 0 is the standard simple_repeater identity, plus one rule that a shared
// radio makes necessary: see RepeaterMesh below.
//
// MyMesh already takes a mesh::Radio&. It does not know whether that is the
// transceiver of the board or one port of a shared transceiver. To reuse it, we
// only give it a RadioPort in place of radio_driver. There is no fork of the
// repeater. There is no #define that renames its class. There are no changes in
// examples/simple_repeater. The build uses MyMesh.cpp from that directory and
// nothing else. Its main.cpp has its own setup() and loop(), so the build
// excludes that file.

#include "HydraSlot.h"
#include <MyMesh.h>          // examples/simple_repeater, on the include path
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>

// The repeater identity, plus the one rule that the shared radio adds to it:
// never relay a frame that a sibling identity on this board transmitted. The
// reasoning and the mechanism are in LoopbackForwardGuard. Everything that
// makes slot 0 a repeater still comes from MyMesh.
//
// This is a subclass and not a change in examples/simple_repeater. The three
// hooks below are stock Dispatcher and Mesh virtuals.
class RepeaterMesh : public MyMesh {
  RadioPort& _port;
  LoopbackForwardGuard _guard;

protected:
  // logRx() runs inside the same recvRaw() as the frame, so the port still
  // reports where that frame came from. onRecvPacket() can run much later.
  void logRx(mesh::Packet* pkt, int len, float score) override {
    MyMesh::logRx(pkt, len, score);
    _guard.onRx(pkt, _port.lastRxWasLoopback());
  }

  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override {
    _guard.beginProcess(pkt);
    mesh::DispatcherAction action = MyMesh::onRecvPacket(pkt);
    _guard.endProcess();
    return action;
  }

  bool allowPacketForward(const mesh::Packet* packet) override {
    if (_guard.blocksForward()) return false;
    return MyMesh::allowPacketForward(packet);
  }

public:
  RepeaterMesh(mesh::MainBoard& b, RadioPort& port, mesh::MillisecondClock& ms,
               mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
      : MyMesh(b, port, ms, rng, rtc, tables), _port(port) {}

  const LoopbackForwardGuard& loopbackGuard() const { return _guard; }
};

class RepeaterSlot : public HydraSlot {
  RadioPort        _port;
  ArduinoMillis    _ms;
  StdRNG           _rng;
  SimpleMeshTables _tables;   // the full 160-hash dedup table: the repeater sees all
  RepeaterMesh     _mesh;
  bool             _begun;

public:
  RepeaterSlot()
      : _mesh(board, _port, _ms, _rng, rtc_clock, _tables), _begun(false) {}

  RadioPort& port() override { return _port; }
  SlotType type() const override { return SLOT_REPEATER; }
  const mesh::LocalIdentity& identity() const override { return _mesh.self_id; }
  // The name of slot 0 is in prefs.json, which CommonCLI owns. It is not in the
  // hydra slot config. Slot 0 is the standard repeater identity, and `set name`
  // still works on it without a slot prefix. ADVERT_NAME makes sure that slot 0
  // always has a name.
  const char* name() const override { return const_cast<RepeaterMesh&>(_mesh).getNodePrefs()->node_name; }
  bool hasPendingWork() const override { return _begun && _mesh.hasPendingWork(); }
  RepeaterMesh& mesh() { return _mesh; }

  bool begin(FILESYSTEM* fs, IdentityStore& store, const char* id_name,
             const char* display_name, SlotType type) override {
    if (!store.load(id_name, _mesh.self_id)) {
      _mesh.self_id = radio_new_identity();
      // the id-hash prefixes 0x00 and 0xFF are reserved
      for (int i = 0; i < 10 && (_mesh.self_id.pub_key[0] == 0x00 || _mesh.self_id.pub_key[0] == 0xFF); i++) {
        _mesh.self_id = radio_new_identity();
      }
      store.save(id_name, _mesh.self_id);
    }
    _mesh.begin(fs);   // this also sends freq, bw, sf, cr and power to the real radio
    _begun = true;
    return true;
  }

  void loop() override { if (_begun) _mesh.loop(); }

  void handleCommand(uint32_t sender_timestamp, char* command,
                     char* reply, size_t reply_sz) override {
    // reply must be >= 160 bytes. The CLI of MyMesh writes into it with no limit.
    if (_begun) _mesh.handleCommand(sender_timestamp, command, reply);
  }

  NodePrefs* prefs() { return _mesh.getNodePrefs(); }
};
