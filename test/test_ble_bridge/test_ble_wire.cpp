#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "helpers/bridges/BleBridgeFrame.h"
#include "helpers/bridges/BleLinkTypes.h"

// What the two transport backends must agree on, byte for byte, or an nRF52
// node and an ESP32 node form a link and then find nothing to talk to.
//
// Neither of these facts is checked by a compiler. The GATT UUIDs are written
// out twice, once as a little-endian byte array for Bluefruit and once as a
// string for NimBLE, and nothing links the two spellings. The advert has to fit
// 31 bytes on both sides, and an overrun is not a soft failure: it makes the
// whole advert invalid and the node then never appears in any scan.

using namespace BleBridgeFrame;

/* ---- The GATT UUIDs ------------------------------------------------------ */

// src/helpers/nrf52/BluefruitLinkBackend.cpp, verbatim. Bluefruit takes a
// 128-bit UUID least significant byte first.
static const uint8_t NRF52_SVC_UUID[16] = {
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
  0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x7A, 0x40, 0x6E
};
static const uint8_t NRF52_CHR_UUID[16] = {
  0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
  0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x7A, 0x40, 0x6E
};

// src/helpers/esp32/NimBleLinkBackend.cpp, verbatim. NimBLE takes a UUID the
// way a person writes one, most significant byte first.
static const char* ESP32_SVC_UUID = "6e407a01-b5a3-f393-e0a9-e50e24dcca9e";
static const char* ESP32_CHR_UUID = "6e407a02-b5a3-f393-e0a9-e50e24dcca9e";

/** The canonical spelling of a UUID that Bluefruit holds back to front. */
static std::string uuidFromLittleEndian(const uint8_t u[16]) {
  char out[37];
  int n = 0;
  for (int i = 15; i >= 0; i--) {
    if (i == 11 || i == 9 || i == 7 || i == 5) out[n++] = '-';
    n += snprintf(&out[n], sizeof(out) - n, "%02x", u[i]);
  }
  out[n] = 0;
  return std::string(out);
}

TEST(BleWireUuid, theServiceIsTheSameOnBothBackends) {
  EXPECT_EQ(uuidFromLittleEndian(NRF52_SVC_UUID), std::string(ESP32_SVC_UUID));
}

TEST(BleWireUuid, theCharacteristicIsTheSameOnBothBackends) {
  EXPECT_EQ(uuidFromLittleEndian(NRF52_CHR_UUID), std::string(ESP32_CHR_UUID));
}

TEST(BleWireUuid, theServiceAndTheCharacteristicDifferInOneSlotOnly) {
  // The base is shared and only the 16-bit slot moves. A change that alters
  // anything else means one of the two spellings was edited alone.
  for (int i = 0; i < 16; i++) {
    if (i == 12) {
      EXPECT_NE(NRF52_SVC_UUID[i], NRF52_CHR_UUID[i]);
    } else {
      EXPECT_EQ(NRF52_SVC_UUID[i], NRF52_CHR_UUID[i]) << "byte " << i;
    }
  }
}

/* ---- The address ---------------------------------------------------------- */

TEST(BleWireAddr, hasTheLayoutBothBackendsPassAround) {
  // On nRF52 BleAddr IS ble_gap_addr_t, and the ESP32 definition copies its
  // layout so the two files read alike. Seven bytes, with the six address bytes
  // last: the tie-break walks addr[5] downwards, and a shifted field would make
  // the two ends of a bridge disagree about which of them dials.
  EXPECT_EQ(sizeof(BleAddr), 7u);
  BleAddr a;
  memset(&a, 0, sizeof(a));
  EXPECT_EQ((size_t)((const uint8_t*)a.addr - (const uint8_t*)&a), 1u);
}

/* ---- The advert ----------------------------------------------------------- */

// The budget both discovery classes compute: 31 bytes in all, 3 for the flags,
// 8 for the beacon block, 2 for the name header, and 18 left for the name.
static const size_t ADV_MAX = 31;
static const size_t NAME_BUDGET = ADV_MAX - 3 - (BEACON_LEN + 2) - 2;

static size_t buildAdvertWithName(uint8_t* ad, uint16_t company, uint16_t marker,
                                  uint8_t batt_dv, const std::string& name) {
  size_t i = 0;
  ad[i++] = 2; ad[i++] = 0x01; ad[i++] = 0x04;              // Flags
  ad[i++] = BEACON_LEN + 1; ad[i++] = 0xFF;                 // Manufacturer data
  buildBeaconRecord(&ad[i], company, marker, batt_dv); i += BEACON_LEN;
  ad[i++] = (uint8_t)(name.size() + 1); ad[i++] = 0x09;     // Complete Local Name
  memcpy(&ad[i], name.data(), name.size()); i += name.size();
  return i;
}

TEST(BleWireAdvert, theNameBudgetIsWhatTheDiscoveryClassesUse) {
  EXPECT_EQ(NAME_BUDGET, 18u);
}

TEST(BleWireAdvert, aNameAtTheFullBudgetStillFitsAndStillParses) {
  FrameCodec c;
  c.setSecret("a-shared-secret");
  uint8_t ad[64];
  std::string name(NAME_BUDGET, 'M');

  size_t len = buildAdvertWithName(ad, 0xFFFF, c.groupMarker(), 37, name);
  EXPECT_EQ(len, ADV_MAX);

  uint8_t batt = 0;
  EXPECT_EQ(parseBeacon(ad, len, 0xFFFF, c.groupMarker(), &batt), BEACON_MATCH);
  EXPECT_EQ(batt, 37);
}

TEST(BleWireAdvert, aNameOneByteOverTheBudgetOverrunsTheAdvert) {
  // Not a soft failure on either stack: an advert above 31 bytes is refused
  // whole, and the node is then invisible to every scanner while it bridges
  // perfectly well. Both discovery classes truncate for exactly this reason.
  FrameCodec c;
  c.setSecret("a-shared-secret");
  uint8_t ad[64];
  std::string name(NAME_BUDGET + 1, 'M');
  EXPECT_GT(buildAdvertWithName(ad, 0xFFFF, c.groupMarker(), 37, name), ADV_MAX);
}

TEST(BleWireAdvert, theBeaconIsFoundWhateverOrderTheStructuresArriveIn) {
  // The two stacks build the same three structures, and neither promises an
  // order. parseBeacon walks them, so a stack that puts the name first must
  // still produce a beacon the other end reads.
  FrameCodec c;
  c.setSecret("a-shared-secret");
  const std::string name = "MeshCore-M5";

  uint8_t ad[64];
  size_t i = 0;
  ad[i++] = (uint8_t)(name.size() + 1); ad[i++] = 0x09;
  memcpy(&ad[i], name.data(), name.size()); i += name.size();
  ad[i++] = 2; ad[i++] = 0x01; ad[i++] = 0x04;
  ad[i++] = BEACON_LEN + 1; ad[i++] = 0xFF;
  buildBeaconRecord(&ad[i], 0xFFFF, c.groupMarker(), 41); i += BEACON_LEN;

  uint8_t batt = 0;
  EXPECT_EQ(parseBeacon(ad, i, 0xFFFF, c.groupMarker(), &batt), BEACON_MATCH);
  EXPECT_EQ(batt, 41);
}
