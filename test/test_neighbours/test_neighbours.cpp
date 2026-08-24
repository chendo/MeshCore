#include <gtest/gtest.h>
#include <vector>
#include <algorithm>
#include <string>

#include <Arduino.h>
#include <helpers/ObserverNeighbours.h>

unsigned long g_fake_millis = 0;

namespace {

const uint8_t FLOOD_HDR = (2 << 2) | 1;   // route=FLOOD, type=TXT_MSG
const uint8_t SELF_KEY[32] = {0x30, 0x70, 0x30, 0x70};

// A flood frame whose path holds the given hop hashes, each `sz` bytes wide.
// The LAST hop is the node whose radio we actually heard.
std::vector<uint8_t> floodWithPath(std::vector<std::vector<uint8_t>> hops, uint8_t sz) {
  std::vector<uint8_t> f{FLOOD_HDR};
  f.push_back((uint8_t)(((sz - 1) << 6) | hops.size()));
  for (auto& h : hops) for (uint8_t i = 0; i < sz; i++) f.push_back(h[i]);
  f.push_back(0xAA);
  return f;
}

struct Obs {
  MeshObserver obs;
  Obs() { obs.reset(); obs.addSelfKey(SELF_KEY); }
  void rx(std::vector<uint8_t> f, int8_t snr4) { obs.observeRx(f.data(), (int)f.size(), snr4); }
};

// A display that records what was drawn instead of drawing it. Fixed-width
// metrics keep the expected geometry arithmetic obvious.
class FakeDisplay : public DisplayDriver {
public:
  static const int CHAR_W = 6;
  struct Draw { int x, y; std::string s; };
  std::vector<Draw> texts;
  int cur_x = 0, cur_y = 0;
  int frames = 0;
  int rects = 0;
  /* Character width in DISPLAY UNITS, which is not panel pixels. The M5 scales
     its 128-unit space onto a 200px panel, so the compact font's 6px cell is
     3.84 units, not 6. Getting this wrong is what makes a layout that measures
     fine on paper truncate on the glass. */
  int char_w = CHAR_W;

  FakeDisplay(int w = 128, int h = 128, int cw = CHAR_W) : DisplayDriver(w, h), char_w(cw) { }

  bool isOn() override { return true; }
  void turnOn() override { }
  void turnOff() override { }
  void clear() override { }
  void startFrame(ColorVal bkg = UIColor::window_bkg) override { texts.clear(); rects = 0; frames++; }
  void setTextSize(int sz) override { }
  void setColor(ColorVal c) override { }
  void setCursor(int x, int y) override { cur_x = x; cur_y = y; }
  void print(const char* str) override { texts.push_back({cur_x, cur_y, str}); }
  void fillRect(int x, int y, int w, int h) override { rects++; }
  void drawRect(int x, int y, int w, int h) override { }
  void drawXbm(int x, int y, const uint8_t* b, int w, int h) override { }
  uint16_t getTextWidth(const char* s) override { return (uint16_t)(strlen(s) * char_w); }
  void endFrame() override { }

  bool has(const std::string& s) const {
    for (auto& d : texts) if (d.s == s) return true;
    return false;
  }
  std::vector<std::string> atY(int y) const {
    std::vector<std::string> out;
    for (auto& d : texts) if (d.y == y) out.push_back(d.s);
    return out;
  }
  int countAtX(int x) const {
    int n = 0;
    for (auto& d : texts) if (d.x == x) n++;
    return n;
  }
};

}  // namespace

// The real display drivers define these; on the host the test does.
ColorVal UIColor::window_bkg = 0, UIColor::title_bkg = 0, UIColor::title_txt = 0;
ColorVal UIColor::primary_txt = 1, UIColor::secondary_txt = 1, UIColor::warning_txt = 1;
ColorVal UIColor::popup_bkg = 0, UIColor::popup_txt = 1, UIColor::corp_blue = 1;

// ---------------------------------------------------------------- filtering

TEST(Neighbours, EveryHopIsListedClosestFirst) {
  Obs o;
  g_fake_millis = 10000;
  // Two hops: 0xAA relayed it, 0xBB transmitted the copy we received. Both are
  // part of this node's picture of the mesh, so both are listed -- but the one
  // whose radio we actually hear comes first.
  o.rx(floodWithPath({{0xAA, 0x01}, {0xBB, 0x02}}, 2), 20);
  ASSERT_EQ(2, o.obs.numPeers());

  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  ASSERT_EQ(2, n.numNeighbours()) << "distant peers are shown too, not filtered out";

  NeighbourRow near, far;
  ASSERT_TRUE(n.getNeighbour(0, near));
  ASSERT_TRUE(n.getNeighbour(1, far));
  EXPECT_EQ(0xBB, near.hash[0]) << "the node we hear directly sorts first";
  EXPECT_EQ(1, near.hops);
  EXPECT_EQ(1u, near.rx);
  EXPECT_TRUE(near.has_snr);
  EXPECT_EQ(0xAA, far.hash[0]);
  EXPECT_EQ(2, far.hops);
  EXPECT_EQ(0u, far.rx) << "we have never heard its radio, and must not imply we have";
  EXPECT_FALSE(far.has_snr);
}

TEST(Neighbours, UnknownDistanceSortsLastNotFirst) {
  Obs o;
  g_fake_millis = 10000;
  o.rx(floodWithPath({{0x11, 0x01}, {0x22, 0x02}}, 2), 20);   // hops 2 and 1
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  ASSERT_EQ(2, n.numNeighbours());
  // min_hops of 0 means "never placed". Sorting it as nearest would put the
  // least-known nodes at the top of a list whose whole point is proximity.
  for (int i = 0; i < n.numNeighbours(); i++) {
    NeighbourRow r;
    n.getNeighbour(i, r);
    EXPECT_NE(0, r.hops) << "row " << i << " should have a placed distance";
  }
  NeighbourRow a, b;
  n.getNeighbour(0, a); n.getNeighbour(1, b);
  EXPECT_LE(a.hops, b.hops) << "hop count must be non-decreasing down the list";
}

TEST(Neighbours, StaleNeighboursDropOffAfterSixHours) {
  Obs o;
  g_fake_millis = 10000;
  o.rx(floodWithPath({{0xBB, 0x02}}, 2), 20);

  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis + ObserverNeighbours::STALE_MS - 1);
  EXPECT_EQ(1, n.numNeighbours()) << "just inside the window";

  n.refresh(g_fake_millis + ObserverNeighbours::STALE_MS + 1);
  EXPECT_EQ(0, n.numNeighbours()) << "six hours silent is not a neighbour";
}

TEST(Neighbours, AMillisWrapDoesNotEvictEveryone) {
  Obs o;
  g_fake_millis = 0xFFFFFF00;                 // moments before the 49-day wrap
  o.rx(floodWithPath({{0xBB, 0x02}}, 2), 20);

  ObserverNeighbours n(o.obs);
  n.refresh(0x00000100);                      // wrapped; ~512 ms later in truth
  EXPECT_EQ(1, n.numNeighbours()) << "unsigned subtraction must survive the wrap";
}

// ------------------------------------------------------------------ ordering

TEST(Neighbours, SortedBySnrDescending) {
  Obs o;
  g_fake_millis = 10000;
  o.rx(floodWithPath({{0x11, 0x01}}, 2), 4);    // +1.0 dB
  o.rx(floodWithPath({{0x22, 0x02}}, 2), 40);   // +10.0 dB
  o.rx(floodWithPath({{0x33, 0x03}}, 2), -20);  // -5.0 dB

  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  ASSERT_EQ(3, n.numNeighbours());

  NeighbourRow a, b, c;
  n.getNeighbour(0, a); n.getNeighbour(1, b); n.getNeighbour(2, c);
  EXPECT_EQ(0x22, a.hash[0]) << "strongest link first";
  EXPECT_EQ(0x11, b.hash[0]);
  EXPECT_EQ(0x33, c.hash[0]);
  EXPECT_EQ(40, a.snr4);
  EXPECT_EQ(-20, c.snr4);
}

// ------------------------------------------------------------------- fields

TEST(Neighbours, ForwardCountUsesConfirmedWidthsOnly) {
  Obs o;
  g_fake_millis = 10000;
  // Our hash then theirs, at 2 bytes: proof they forwarded our packet.
  o.rx(floodWithPath({{0x30, 0x70}, {0xBB, 0x02}}, 2), 20);

  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  ASSERT_EQ(1, n.numNeighbours());
  NeighbourRow p;
  n.getNeighbour(0, p);
  EXPECT_EQ(1u, p.fwd);

  // The same shape at 1 byte is a 1-in-256 coincidence and must not be counted.
  Obs g;
  g_fake_millis = 10000;
  g.rx(floodWithPath({{0x30}, {0xCC}}, 1), 20);
  ObserverNeighbours m(g.obs);
  m.refresh(g_fake_millis);
  ASSERT_EQ(1, m.numNeighbours());
  m.getNeighbour(0, p);
  EXPECT_EQ(0u, p.fwd) << "one byte is not proof, so it must not inflate FWD";
}

TEST(Neighbours, NameIsEmptyUntilAnAdvertArrives) {
  Obs o;
  g_fake_millis = 10000;
  o.rx(floodWithPath({{0xBB, 0x02}}, 2), 20);
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  NeighbourRow p;
  n.getNeighbour(0, p);
  ASSERT_NE(nullptr, p.name);
  EXPECT_STREQ("", p.name);
}

TEST(Neighbours, NamesHaveRoomForAFullNodeName) {
  // The field must hold VIC-NorthcoteNW-EDG-01 and longer without clipping the
  // tail, which is the part that distinguishes sibling nodes.
  EXPECT_GE((int)MeshObserver::NAME_LEN, 32);
  MeshObserver::PeerEntry e;
  EXPECT_EQ((size_t)MeshObserver::NAME_LEN, sizeof(e.name));
}

// -------------------------------------------------------------------- screen

class ScreenTest : public ::testing::Test {
protected:
  Obs o;
  FakeDisplay d;
  void SetUp() override { g_fake_millis = 10000; }
};

TEST_F(ScreenTest, RowCapacityLeavesRoomForTheHeader) {
  ObserverNeighbours n(o.obs);
  NeighboursScreen s(n, "NODE", "915.0", 11);
  // 128 tall, 2 + 4*11 = 46 of header (name, config, rule, labels), 11 per row.
  EXPECT_EQ((128 - 46) / 11, s.rowCapacity(d));
  FakeDisplay small(128, 64);
  EXPECT_EQ((64 - 46) / 11, s.rowCapacity(small));
}

TEST_F(ScreenTest, DrawsOneRowPerNeighbourWithHeaders) {
  o.rx(floodWithPath({{0x11, 0x01}}, 2), 40);
  o.rx(floodWithPath({{0x22, 0x02}}, 2), 4);

  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  NeighboursScreen s(n, "VIC-NorthcoteNW-EDG-01", "915.075 SF9", 11);
  int next = s.render(d);

  EXPECT_GT(next, 0) << "render must ask to be called again";
  EXPECT_EQ(1, d.frames);
  EXPECT_TRUE(d.has("NAME"));
  EXPECT_TRUE(d.has("SNR"));
  EXPECT_TRUE(d.has("RX"));
  EXPECT_TRUE(d.has("FWD"));
  EXPECT_TRUE(d.has("HOP")) << "the sort is by distance, so distance must be visible";
  EXPECT_TRUE(d.has("1")) << "a directly-heard peer renders as hop 1";
  EXPECT_TRUE(d.has("915.075 SF9"));
  EXPECT_TRUE(d.has("+10.0")) << "quarter-dB means render as signed tenths";
  EXPECT_TRUE(d.has("+1.0"));
  // Unnamed peers fall back to their path hash rather than a blank row.
  EXPECT_TRUE(d.has("1101"));
  EXPECT_TRUE(d.has("2202"));
}

TEST_F(ScreenTest, NegativeSnrKeepsItsSignBelowOneDb) {
  o.rx(floodWithPath({{0x11, 0x01}}, 2), -2);   // -0.5 dB
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  NeighboursScreen s(n, "N", "cfg", 11);
  s.render(d);
  EXPECT_TRUE(d.has("-0.5")) << "the sign must survive integer division to zero";
}

TEST_F(ScreenTest, EmptyTableSaysSoRatherThanLookingBroken) {
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  NeighboursScreen s(n, "N", "cfg", 11);
  s.render(d);
  EXPECT_TRUE(d.has("no neighbours"));
}

TEST_F(ScreenTest, TruncationIsAlwaysDisclosed) {
  // More neighbours than a short display can hold.
  for (uint8_t i = 0; i < 12; i++) {
    o.rx(floodWithPath({{(uint8_t)(0x40 + i), 0x01}}, 2), (int8_t)(40 - i));
  }
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  ASSERT_EQ(12, n.numNeighbours());

  FakeDisplay small(128, 64);
  NeighboursScreen s(n, "N", "cfg", 11);
  s.render(small);

  int cap = s.rowCapacity(small);
  ASSERT_LT(cap, 12);
  char badge[16];
  snprintf(badge, sizeof(badge), "+%d", 12 - cap);
  EXPECT_TRUE(small.has(badge)) << "a truncated list must never read as complete";
}

TEST_F(ScreenTest, ColumnsAreRightAlignedAndDoNotOverlapTheName) {
  o.rx(floodWithPath({{0x11, 0x01}}, 2), 40);
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  NeighboursScreen s(n, "N", "cfg", 11);
  s.render(d);

  // FWD's right edge is the display edge, so its label starts one width in.
  int x_fwd_label = 128 - (int)strlen("FWD") * FakeDisplay::CHAR_W;
  bool found = false;
  for (auto& t : d.texts) if (t.s == "FWD" && t.x == x_fwd_label) found = true;
  EXPECT_TRUE(found) << "FWD must be flush with the right edge";

  // Only the title, the divider, the NAME label and the names sit at x=0.
  for (auto& t : d.texts) {
    if (t.x != 0) continue;
    bool rule = t.s.size() > 1 && t.s.find_first_not_of('-') == std::string::npos;
    EXPECT_TRUE(rule || t.s == "NAME" || t.s == "1101" || t.s == "N") << t.s;
  }
}

/* The M5's e-paper as the driver reports it once the compact font is in play:
   a 128x128 unit space minus the 10 units of EINK_Y_OFFSET that are shifted off
   the bottom, and 6 units of pitch. This is the geometry the layout was drawn
   for, so it is worth pinning: a regression here silently costs peer rows. */
TEST(CompactGeometry, TheM5FitsAUsableNumberOfPeers) {
  Obs o;
  g_fake_millis = 10000;
  for (uint8_t i = 0; i < 20; i++) {
    o.rx(floodWithPath({{(uint8_t)(0x40 + i), 0x01}}, 2), (int8_t)(40 - i));
  }
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);
  ASSERT_EQ(20, n.numNeighbours());

  // 6 panel px / 1.5625 scale = 3.84 units per character, rounded to 4.
  FakeDisplay m5(128, 118, 4);
  NeighboursScreen s(n, "VIC-NorthcoteNW-EDG-01", "915.075 BW125 SF9 CR5", 6, 5000, 0);
  EXPECT_EQ(15, s.rowCapacity(m5)) << "the compact font must buy back peer rows";

  s.render(m5);
  // A full node name has to survive at this density; that was the whole point.
  EXPECT_TRUE(m5.has("VIC-NorthcoteNW-EDG-01"));
  EXPECT_TRUE(m5.has("+5")) << "the truncation badge, 20 neighbours into 15 rows";
}

/* The bug this pins: at a pitch of 11 the header block's hand-rolled offsets
   happened to clear each other, and at 6 they did not -- the column labels were
   drawn ONE unit above the first peer row, so the two printed on top of one
   another on the glass. Nothing in the geometry tests noticed, because they
   only ever asked how many rows fit. Assert the thing that was actually wrong:
   no two text lines may sit closer together than one pitch. */
TEST(Layout, NoTwoLinesAreCloserThanOnePitch) {
  for (int pitch : {5, 6, 8, 11}) {
    Obs o;
    g_fake_millis = 10000;
    for (uint8_t i = 0; i < 6; i++) {
      o.rx(floodWithPath({{(uint8_t)(0x50 + i), 0x02}}, 2), (int8_t)(30 - i));
    }
    ObserverNeighbours n(o.obs);
    n.refresh(g_fake_millis);

    FakeDisplay d(128, 118, 4);
    NeighboursScreen s(n, "VIC-NorthcoteNW-EDG-01", "915.075 BW125 SF9 CR5", pitch, 5000, 0);
    s.render(d);

    std::vector<int> ys;
    for (auto& t : d.texts) ys.push_back(t.y);
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    ASSERT_GT(ys.size(), 3u) << "pitch " << pitch << ": expected header plus rows";
    for (size_t i = 1; i < ys.size(); i++) {
      EXPECT_GE(ys[i] - ys[i-1], pitch)
          << "pitch " << pitch << ": lines at y=" << ys[i-1] << " and y=" << ys[i]
          << " would overlap on the glass";
    }
    EXPECT_LE(ys.back(), 118) << "pitch " << pitch << ": last line runs off the panel";
  }
}

/* The divider must be text. GxEPDDisplay's setCursor() adds EINK_Y_OFFSET and a
   baseline correction while fillRect() adds neither, so a rect positioned
   against a text baseline lands 15.6px away from where it was meant to go --
   which is how the rule ended up struck through the column labels. */
TEST(Layout, TheDividerIsDrawnAsTextNotAsARect) {
  Obs o;
  g_fake_millis = 10000;
  o.rx(floodWithPath({{0x11, 0x01}}, 2), 20);
  ObserverNeighbours n(o.obs);
  n.refresh(g_fake_millis);

  FakeDisplay d(128, 118, 4);
  NeighboursScreen s(n, "N", "cfg", 6, 5000, 0);
  s.render(d);
  EXPECT_EQ(0, d.rects) << "no rect may be positioned against a text baseline";

  bool rule = false;
  for (auto& t : d.texts) {
    if (t.s.size() > 4 && t.s.find_first_not_of('-') == std::string::npos) rule = true;
  }
  EXPECT_TRUE(rule) << "the divider row of dashes must be drawn";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
