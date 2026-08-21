#include "BleStack.h"

#include <bluefruit.h>

namespace BleStack {

static bool s_done = false;
static uint8_t s_prph = 1, s_central = 0;
static uint16_t s_mtu = 23;
static uint8_t s_qsize = 1;

bool ensure(const char* name, uint8_t prph, uint8_t central) {
  if (!s_done) {
    /* Start the stack only if nobody started it. A second Bluefruit.begin()
       fails with INVALID_STATE and stops the caller too. */
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (!sd_enabled) {
      if (prph < 1) prph = 1;                    // the CLI always needs one

      /* Step down until the RAM that the SoftDevice needs fits the RAM that
         the linker reserved. Do not guess the limit at build time. Measured on
         a RAK3401: (4,3) does not fit the 24KB at RAM origin 0x20006000, and
         the node came up with one peripheral only. The true ceiling is thus
         lower than it looks, so find it instead of an assumption.

         Buffers first, connection slots second.

         The link had a buffer shortage. The write-command and notify queues of
         the SoftDevice hold ONE packet by default
         (BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT == 1). The stack thus
         refused the second chunk of every fragmented frame, and the sender
         abandoned that frame with its header already on the air. A deeper queue
         lets the fragments go out back to back. A large MTU is better again: at
         MTU 247 a bridge frame of up to 244 bytes is ONE write, so it never
         becomes fragments and the whole class of reassembly faults cannot
         occur.

         The outer loop thus selects the buffer tier and the inner loop gives up
         slots. One link with a real MTU is better than three links that
         fragment everything. The floor is the exception: before the last tier
         we never go below (1,1), because a node with no outward link is not a
         bridge, and a bridge that fragments beats a fast non-bridge.

         Each failed attempt leaves the SoftDevice enabled but not configured,
         because sd_softdevice_enable() succeeds before the configuration
         fails. The SoftDevice must thus stop before the next attempt, or that
         attempt fails with INVALID_STATE. */
      struct Tier { uint16_t mtu; uint8_t qsize; };
      static const Tier TIERS[] = {
        { 247, 7 },      // nothing that we send needs fragments
        { 185, 4 },      // most frames are one write
        { 123, 3 },
        {  69, 2 },
        {  23, 1 },      // stock: every frame becomes fragments, queue of one
      };
      const uint8_t NUM_TIERS = (uint8_t)(sizeof(TIERS) / sizeof(TIERS[0]));

      const uint8_t req_prph = prph, req_central = central;
      bool up = false;

      /* The lowest tier that repairs the queue-of-one fault. A queue of 3 lets
         fragments go out back to back, and MTU 123 carries a frame of 105 bytes
         in one write, which is most bridge traffic. Each tier above this one
         adds throughput, not correctness.

         MEASURED on a RAK3401: the top tier (247/q7) fitted at 1p/1c only. That
         price is too high. One peripheral slot puts the CLI and an inbound peer
         link in competition, so a person who connects to manage the node locks
         its peer out. A slot is a capability; an MTU above tier 2 is an
         optimisation.

         Thus: hold the requested slots and take the best buffers that fit
         beside them, down to LAST_GOOD_TIER. Trade slots away only when even
         that tier does not fit. Accept the poor tiers, which are the old stock
         behaviour, only when the slots are gone. */
      const uint8_t LAST_GOOD_TIER = 2;          // 123 / q3

      for (uint8_t pass = 0; pass < 2 && !up; pass++) {
        const uint8_t first_tier = (pass == 0) ? 0 : (uint8_t)(LAST_GOOD_TIER + 1);
        const uint8_t last_tier  = (pass == 0) ? LAST_GOOD_TIER : (uint8_t)(NUM_TIERS - 1);
        if (first_tier >= NUM_TIERS) break;

        uint8_t p = req_prph, c = req_central;
        for (;;) {
          for (uint8_t tier = first_tier; tier <= last_tier && !up; tier++) {
            Bluefruit.configPrphConn(TIERS[tier].mtu, BLE_GAP_EVENT_LENGTH_DEFAULT,
                                     TIERS[tier].qsize, TIERS[tier].qsize);
            Bluefruit.configCentralConn(TIERS[tier].mtu, BLE_GAP_EVENT_LENGTH_DEFAULT,
                                        TIERS[tier].qsize, TIERS[tier].qsize);
            if (Bluefruit.begin(p, c)) {
              s_prph = p; s_central = c;
              s_mtu = TIERS[tier].mtu; s_qsize = TIERS[tier].qsize;
              up = true;
            } else {
              /* The failed attempt left the SoftDevice enabled but not
                 configured. Stop it, or the next attempt fails with
                 INVALID_STATE. */
              sd_softdevice_disable();
            }
          }
          if (up) break;

          /* Give up the spare OUTBOUND slots first, then the inbound slot, and
             give up the last outward link in the second pass only. Measured on
             a RAK3401: the stack refused (4,3) down to (4,0) and accepted
             (3,0). About three connections thus fit at stock buffers, and fewer
             at real ones. The node keeps its one remaining peripheral slot for
             the CLI, so a person can always reach it. A peer that we cannot
             accept inward dials US instead, which is why the last central slot
             is the one to keep. */
          const uint8_t min_central = (pass == 1) ? 0 : 1;
          if (c > 1)                c--;
          else if (p > 1)           p--;
          else if (c > min_central) c--;
          else break;                            // exhausted; let the next pass try
        }
      }
      if (!up) return false;                     // even (1,0) at stock buffers: no BLE
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
