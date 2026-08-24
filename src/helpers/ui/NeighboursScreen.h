#pragma once

#include "UIScreen.h"

// Long enough for a full 32-char node_name plus formatting slack.
#define NEIGHBOUR_TMP_LEN 48

/**
 * \brief  A dense one-row-per-neighbour table.
 *
 * The screen holds no mesh types on purpose. It draws whatever a
 * NeighbourSource hands it, so it compiles against the UI layer alone and can
 * be unit-tested on the host with a stub source.
 *
 * Nothing here assumes a character grid. Column positions come from
 * getTextWidth() and the row count comes from height(), so the same code lays
 * out correctly on a 200x200 e-paper with a proportional font and on a 128x64
 * OLED with a fixed one. It draws as many rows as fit and no more.
 */
/* NeighbourRow, not NeighbourInfo: upstream's examples/simple_repeater/MyMesh.h
   already owns that name at global scope. */
struct NeighbourRow {
  const char* name;      // may be null or empty when the node has not adverted
  const uint8_t* hash;   // path-hash bytes, the fallback label when name is empty
  uint8_t hash_len;
  int32_t snr4;          // mean SNR in quarter-dB
  bool has_snr;
  uint8_t hops;          // 1 = we hear its radio; 0 = not known
  uint32_t rx;           // frames received from it directly
  uint32_t fwd;          // times it was seen forwarding one of OUR packets
};

class NeighbourSource {
public:
  virtual ~NeighbourSource() { }
  /** How many rows are on offer, already filtered to direct neighbours and
   *  already ordered best-first. The screen does not sort or filter. */
  virtual int numNeighbours() = 0;
  virtual bool getNeighbour(int i, NeighbourRow& out) = 0;
};

class NeighboursScreen : public UIScreen {
  NeighbourSource& _src;
  const char* _title;      // our own node name
  const char* _subtitle;   // the radio config, drawn right-aligned
  int _pitch;
  int _refresh_ms;
  int _text_size;

public:
  /** \param pitch      baseline-to-baseline spacing in display units. 11 matches
   *                    what the other MeshCore screens use at text size 1.
   *  \param text_size  passed straight to the driver. A driver offering a
   *                    compact font at 0 roughly triples the row count, so a
   *                    caller on a dense panel wants 0 and a smaller pitch. */
  NeighboursScreen(NeighbourSource& src, const char* title, const char* subtitle,
                   int pitch = 11, int refresh_ms = 5000, int text_size = 1)
    : _src(src), _title(title), _subtitle(subtitle), _pitch(pitch),
      _refresh_ms(refresh_ms), _text_size(text_size) { }

  void setTitle(const char* t) { _title = t; }
  void setSubtitle(const char* s) { _subtitle = s; }

  /** How many neighbour rows would fit on this display. Exposed so a caller
   *  can page, and so a test can assert the geometry without a framebuffer. */
  int rowCapacity(DisplayDriver& display) const;

  int render(DisplayDriver& display) override;
};
