#include "BleStack.h"

#include <bluefruit.h>

namespace BleStack {

static bool s_done = false;
static uint8_t s_prph = 1, s_central = 0;
static uint16_t s_mtu = 23;
static uint8_t s_qsize = 1;

bool ensure(const char* name, uint8_t prph, uint8_t central) {
  if (!s_done) {
    /* Only bring it up if nobody already has -- a second Bluefruit.begin()
       would fail on INVALID_STATE and take the caller down with it. */
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (!sd_enabled) {
      if (prph < 1) prph = 1;                    // the CLI always needs one

      /* Walk down until the SoftDevice's RAM requirement fits what the linker
         reserved, rather than guessing at build time. Measured on a RAK3401:
         (4,3) does not fit the 24KB at RAM origin 0x20006000 and the node came
         up with a single peripheral, so the ceiling is genuinely lower than it
         looks and worth discovering rather than assuming.

         Each failed attempt leaves the SoftDevice enabled but unconfigured --
         sd_softdevice_enable() succeeds before the config does not -- so it has
         to come down before the next try or that one fails on INVALID_STATE. */
      /* Buffer tier, shed BEFORE connection slots.

         The link's fragmentation trouble was really a buffer shortage: the
         SoftDevice's write-command and notify queues default to ONE packet
         (BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT == 1), so the second chunk of
         any fragmented frame was refused outright and the frame abandoned with
         its header already sent. A deeper queue lets the fragments queue; a
         larger MTU is better still, because most bridge frames then fit in a
         single write and are never fragmented at all.

         Both cost SoftDevice RAM from the same 24KB that is already tight
         enough to be shedding connection slots -- see below -- so they are
         asked for first and given up first. Losing MTU costs throughput; losing
         a central slot costs the peer link entirely, which is the thing all of
         this exists to fix. The ORDER matters more than either value. */
      struct Tier { uint16_t mtu; uint8_t qsize; };
      static const Tier TIERS[] = {
        { 247, 7 },      // nothing we send needs fragmenting
        { 185, 4 },      // most frames single-write
        { 123, 3 },
        {  69, 2 },
        {  23, 1 },      // stock: fragments everything, queue of one
      };
      const uint8_t NUM_TIERS = (uint8_t)(sizeof(TIERS) / sizeof(TIERS[0]));
      uint8_t tier = 0;

      bool up = false;
      while (!up) {
        Bluefruit.configPrphConn(TIERS[tier].mtu, BLE_GAP_EVENT_LENGTH_DEFAULT,
                                 TIERS[tier].qsize, TIERS[tier].qsize);
        Bluefruit.configCentralConn(TIERS[tier].mtu, BLE_GAP_EVENT_LENGTH_DEFAULT,
                                    TIERS[tier].qsize, TIERS[tier].qsize);
        if (Bluefruit.begin(prph, central)) {
          s_prph = prph;
          s_central = central;
          s_mtu = TIERS[tier].mtu;
          s_qsize = TIERS[tier].qsize;
          up = true;
          break;
        }
        sd_softdevice_disable();

        /* Buffers before connections. */
        if (tier + 1 < NUM_TIERS) { tier++; continue; }

        /* Out of buffer tiers: now shed slots, and retry the whole buffer
           ladder at the smaller connection count -- a slot freed may pay for a
           bigger MTU, which is the trade we actually want. */
        tier = 0;
        /* Shed INBOUND links first and keep the outbound ones. Measured on a
           RAK3401: (4,3) down to (4,0) were all refused and (3,0) accepted, so
           roughly three connections fit -- but spending all three on peers
           dialling in leaves none to dial out with, which is the half that
           actually needs a slot reserved. A node can always be reached on its
           one remaining peripheral slot (the CLI), and a peer that cannot
           connect inward to us will be connected to BY us instead. */
        if (prph > 1) {
          prph--;
        } else if (central > 0) {
          central--;
        } else {
          return false;                          // even (1,0) refused: no BLE at all
        }
      }
    }
    s_done = true;
  }
  if (name != nullptr) Bluefruit.setName(name);
  return true;
}

uint8_t periphSlots()  { return s_prph; }
uint8_t centralSlots() { return s_central; }
uint16_t mtu()         { return s_mtu; }
uint8_t txQueueSize()  { return s_qsize; }

}
