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
      /* MTU first, connection slots second.

         The link's trouble was a buffer shortage: the SoftDevice's write-command
         and notify queues default to ONE packet
         (BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT == 1), so the second chunk of
         any fragmented frame was refused outright and the frame abandoned with
         its header already on the wire. A deeper queue lets fragments pipeline.
         A large MTU is better still: at 247 a bridge frame up to 229 bytes is a
         SINGLE write, so it is never fragmented and the entire class of
         reassembly bug cannot occur.

         So the outer loop is the buffer tier and the inner loop sheds slots --
         we would rather run one link with a real MTU than three that fragment
         everything. The exception is the floor: until the last tier we never
         shed below (1,1), because a node with no outward link is not a bridge,
         and a fragmenting bridge beats a fast non-bridge. */
      struct Tier { uint16_t mtu; uint8_t qsize; };
      static const Tier TIERS[] = {
        { 247, 7 },      // nothing we realistically send needs fragmenting
        { 185, 4 },      // most frames single-write
        { 123, 3 },
        {  69, 2 },
        {  23, 1 },      // stock: fragments everything, queue of one
      };
      const uint8_t NUM_TIERS = (uint8_t)(sizeof(TIERS) / sizeof(TIERS[0]));

      const uint8_t req_prph = prph, req_central = central;
      bool up = false;

      for (uint8_t tier = 0; tier < NUM_TIERS && !up; tier++) {
        Bluefruit.configPrphConn(TIERS[tier].mtu, BLE_GAP_EVENT_LENGTH_DEFAULT,
                                 TIERS[tier].qsize, TIERS[tier].qsize);
        Bluefruit.configCentralConn(TIERS[tier].mtu, BLE_GAP_EVENT_LENGTH_DEFAULT,
                                    TIERS[tier].qsize, TIERS[tier].qsize);

        /* Only the last tier -- which is the old stock behaviour anyway -- may
           give up the outward link entirely. */
        const uint8_t min_central = (tier + 1 == NUM_TIERS) ? 0 : 1;

        uint8_t p = req_prph, c = req_central;
        for (;;) {
          if (Bluefruit.begin(p, c)) {
            s_prph = p; s_central = c;
            s_mtu = TIERS[tier].mtu; s_qsize = TIERS[tier].qsize;
            up = true;
            break;
          }
          sd_softdevice_disable();

          /* Shed OUTBOUND spares first, then the inbound slot, and only as a
             last resort the final outward link. Measured on a RAK3401: (4,3)
             down to (4,0) were all refused and (3,0) accepted, so roughly three
             connections fit at stock buffers -- fewer with real ones. A node is
             always reachable on its one remaining peripheral slot (the CLI),
             and a peer we cannot accept inward will be dialled BY us instead --
             which is exactly why the last central slot is the one to keep. */
          if (c > 1)               c--;
          else if (p > 1)          p--;
          else if (c > min_central) c--;
          else break;                            // this tier cannot fit; try a smaller one
        }
      }
      if (!up) return false;                     // even (1,0) at stock buffers: no BLE at all
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
