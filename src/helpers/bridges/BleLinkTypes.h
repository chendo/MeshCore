#pragma once

#include <stdint.h>

/**
 * @brief  The few things that BleLink and its transport backend must agree on.
 *
 * BleLink carries the bridge frames and holds no stack code. A backend supplies
 * the stack: BluefruitLinkBackend on nRF52, NimBleLinkBackend on ESP32. Both
 * sides need the address type and the link count, so they live here rather than
 * in either one.
 */

/* Outward links. Bounded by the central connections that the stack grants,
   which on a RAK3401 came to three. The backend sizes its own per-link objects
   from this, so it cannot be a member of BleLink. */
#ifndef BLE_LINK_MAX_LINKS
#define BLE_LINK_MAX_LINKS 3
#endif

/**
 * @brief  A BLE device address.
 *
 * On nRF52 this IS the SoftDevice type. The alias, and not a struct of our own,
 * is what lets the backend hand the same bytes to sd_ble_gap_addr_get() and to
 * Bluefruit.Central.connect() that it did before this seam existed, with no
 * conversion at any call site.
 *
 * The ESP32 definition below has the same seven bytes in the same order, so a
 * person who reads one build reads the other. The address itself is
 * little-endian on the air, which is why every comparison in BleLink walks it
 * from addr[5] downwards.
 */
#ifdef NRF52_PLATFORM
  #include <bluefruit.h>
  typedef ble_gap_addr_t BleAddr;
#else
  struct BleAddr {
    uint8_t addr_id_peer : 1;
    uint8_t addr_type : 7;
    uint8_t addr[6];
  };
#endif
