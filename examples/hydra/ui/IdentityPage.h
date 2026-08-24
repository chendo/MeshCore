#pragma once

#include <helpers/ui/PagedScreen.h>
#include <Utils.h>
#include "../HydraNode.h"

/* One slot: who it is, what it has moved, and a way to make it advert.
 *
 * The page holds only a slot index. Everything shown is read live, because a
 * slot can be enabled, renamed or taken down from the CLI while its page is on
 * screen, and a cached copy would quietly go stale.
 */
class IdentityPage : public UIPage {
  int _idx;
  int _pitch;
  PagedScreen* _owner;      // for the toast; the page does not own the frame

  void row(DisplayDriver& d, int y, const char* left, const char* right) {
    d.drawTextLeftAlign(0, y, left);
    if (right) d.drawTextRightAlign(d.width(), y, right);
  }

public:
  IdentityPage(int idx, int pitch, PagedScreen* owner)
    : _idx(idx), _pitch(pitch), _owner(owner) { }

  int slotIndex() const { return _idx; }

  int renderBody(DisplayDriver& display, int avail_h) override {
    char l[40], r[28];
    const int W = display.width();
    int y = 2;

    HydraSlot* slot = hydra.slot(_idx);
    const bool live = hydra.radio().portActive(_idx);

    snprintf(l, sizeof(l), "[%d] %s", _idx, slot ? HydraNode::typeName(slot->type()) : "?");
    display.setColor(UIColor::primary_txt);
    display.drawTextLeftAlign(0, y, l);
    display.drawTextRightAlign(W, y, live ? "up" : "down");
    y += _pitch;

    // The name, which is the thing a person actually recognises, gets its own
    // full-width line rather than being squeezed beside a label.
    const char* nm = (slot && slot->name() && slot->name()[0]) ? slot->name() : "(unnamed)";
    display.translateUTF8ToBlocks(l, nm, sizeof(l));
    display.drawTextEllipsized(0, y, W, l);
    y += _pitch;

    const int dash_w = display.getTextWidth("-");
    int dashes = dash_w > 0 ? W / dash_w : 0;
    if (dashes > (int)sizeof(l) - 1) dashes = (int)sizeof(l) - 1;
    for (int i = 0; i < dashes; i++) l[i] = '-';
    l[dashes > 0 ? dashes : 0] = 0;
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(0, y, l);
    display.setColor(UIColor::primary_txt);
    y += _pitch;

    if (live && slot) {
      char key[9] = "-";
      mesh::Utils::toHex(key, slot->identity().pub_key, 4);
      row(display, y, "key", key);
      y += _pitch;
    }

    snprintf(l, sizeof(l), "rx %u", (unsigned)hydra.radio().portRxCount(_idx));
    snprintf(r, sizeof(r), "tx %u", (unsigned)hydra.radio().portTxCount(_idx));
    row(display, y, l, r);
    y += _pitch;

    if (y + _pitch <= avail_h) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(0, y, live ? "hold B2: advert" : "slot is down");
      display.setColor(UIColor::primary_txt);
    }
    return 5000;
  }

  /** A hold on button 2 floods an advert for THIS identity.
   *
   *  It goes out through the ordinary command path rather than by reaching into
   *  the slot: `advert` is CommonCLI's flood advert (advert.zerohop is the other
   *  one), the per-slot form is what the console already uses, and the reply it
   *  produces is the toast text. So the button can do exactly what the operator
   *  can do, no more, and it reports the same words back. */
  bool handleInput(char c) override {
    if (c != KEY_CONTEXT_MENU) return false;      // btn2 hold
    if (!hydra.radio().portActive(_idx)) {
      if (_owner) _owner->showToast("slot is down", millis());
      return true;
    }
    char cmd[24], reply[96];
    if (_idx == 0) snprintf(cmd, sizeof(cmd), "advert");
    else           snprintf(cmd, sizeof(cmd), "slot %d advert", _idx);
    reply[0] = 0;
    // sender_timestamp 0 == came from the console, which is what a button is.
    hydra.handleCommand(0, cmd, reply, sizeof(reply));
    if (_owner) _owner->showToast(reply[0] ? reply : "advert sent", millis());
    return true;
  }
};
