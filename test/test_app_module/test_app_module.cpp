/* The registry is what an entrypoint trusts, so what it must never do is lose a
   module, run one twice, or hand an identity's packets to something that has no
   identity. These are the properties main.cpp cannot check for itself. */

#include <gtest/gtest.h>
#include "helpers/AppModule.h"
#include <vector>
#include <string>

/* A module that records what it was asked to do, so order and delivery are
   observable rather than assumed. */
class Recorder : public AppModule {
  AppScope _scope;
public:
  std::string tag;
  std::vector<std::string>* log;
  int setups = 0, loops = 0, sent = 0, recv = 0;
  uint8_t last_idx = 255;
  bool pending = false;

  Recorder(const char* t, AppScope s, std::vector<std::string>* l)
      : _scope(s), tag(t), log(l) {}

  AppScope scope() const override { return _scope; }
  void onSetup() override { setups++; if (log) log->push_back(tag + ":setup"); }
  void onLoop() override { loops++; if (log) log->push_back(tag + ":loop"); }
  bool hasPendingWork() const override { return pending; }
  void onPacketSent(uint8_t idx, mesh::Packet*, int) override { sent++; last_idx = idx; }
  void onPacketRecv(uint8_t idx, mesh::Packet*, int, float) override { recv++; last_idx = idx; }
};

/* The registry is static and the cases share one process, so each starts from
   empty. AppModules::reset() is the supported way to do that. */
struct AppModuleRegistry : public ::testing::Test {
  void SetUp() override { AppModules::reset(); }
};

TEST_F(AppModuleRegistry, runsEveryModuleOnceInRegistrationOrder) {
  std::vector<std::string> log;
  Recorder a("a", AppScope::NODE, &log), b("b", AppScope::MESH, &log);
  ASSERT_TRUE(AppModules::add(&a));
  ASSERT_TRUE(AppModules::add(&b));

  AppModules::setupAll();
  AppModules::loopAll();

  EXPECT_EQ(a.setups, 1);
  EXPECT_EQ(b.setups, 1);
  EXPECT_EQ(a.loops, 1);
  EXPECT_EQ(b.loops, 1);
  // registration order, not reverse and not arbitrary
  ASSERT_EQ(log.size(), 4u);
  EXPECT_EQ(log[0], "a:setup");
  EXPECT_EQ(log[1], "b:setup");
  EXPECT_EQ(log[2], "a:loop");
  EXPECT_EQ(log[3], "b:loop");
}

TEST_F(AppModuleRegistry, packetsReachMeshScopeOnly) {
  /* The whole point of the scope split: a node module has no identity, so
     handing it one identity's packets would be asking it the wrong question. */
  Recorder node("n", AppScope::NODE, nullptr), meshm("m", AppScope::MESH, nullptr);
  AppModules::add(&node);
  AppModules::add(&meshm);

  AppModules::packetRecv(3, nullptr, 10, 1.5f);
  AppModules::packetSent(3, nullptr, 10);

  EXPECT_EQ(node.recv, 0);
  EXPECT_EQ(node.sent, 0);
  EXPECT_EQ(meshm.recv, 1);
  EXPECT_EQ(meshm.sent, 1);
}

TEST_F(AppModuleRegistry, packetsCarryTheIdentityTheyBelongTo) {
  /* A hydra node runs several meshes. A callback that cannot say which one is
     useless, and retrofitting the index later means touching every module. */
  Recorder m("m", AppScope::MESH, nullptr);
  AppModules::add(&m);

  AppModules::packetRecv(2, nullptr, 8, 0.0f);
  EXPECT_EQ(m.last_idx, 2);
  AppModules::packetRecv(0, nullptr, 8, 0.0f);
  EXPECT_EQ(m.last_idx, 0);
}

TEST_F(AppModuleRegistry, anyPendingWorkIsAnOrOverModules) {
  /* This gates the powersave sleep. One module with work must keep the whole
     node awake, or that module's work is what the sleep destroys. */
  Recorder a("a", AppScope::NODE, nullptr), b("b", AppScope::MESH, nullptr);
  AppModules::add(&a);
  AppModules::add(&b);

  EXPECT_FALSE(AppModules::anyPendingWork());
  b.pending = true;
  EXPECT_TRUE(AppModules::anyPendingWork());
  b.pending = false;
  a.pending = true;
  EXPECT_TRUE(AppModules::anyPendingWork());
}

TEST_F(AppModuleRegistry, refusesNullAndRefusesToOverflow) {
  /* A full registry must fail visibly. Silently dropping the last module is a
     feature that is simply absent at runtime, on one board, in one build. */
  EXPECT_FALSE(AppModules::add(nullptr));

  std::vector<Recorder*> made;
  while (AppModules::count() < APP_MODULE_MAX) {
    Recorder* r = new Recorder("fill", AppScope::NODE, nullptr);
    made.push_back(r);
    ASSERT_TRUE(AppModules::add(r));
  }
  Recorder overflow("over", AppScope::NODE, nullptr);
  EXPECT_FALSE(AppModules::add(&overflow));
  EXPECT_EQ(AppModules::count(), APP_MODULE_MAX);

  for (auto* r : made) delete r;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
