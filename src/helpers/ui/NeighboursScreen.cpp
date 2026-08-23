#include "NeighboursScreen.h"

#include <stdio.h>
#include <string.h>

// The three header lines: our name, the radio config, then the column labels
// with a rule above them. Row 0 of the table sits one pitch below the labels.
static const int TITLE_Y = 2;

/* Format a quarter-dB mean as tenths, without pulling in float printf.
   The mean is taken FIRST so the multiply can never overflow: a quarter-dB
   mean fits in a couple of hundred, whatever the sample count. */
static void fmtSnr(char* out, size_t n, int32_t snr4, bool has) {
  if (!has) { snprintf(out, n, "-"); return; }
  int32_t tenths = (snr4 * 10) / 4;
  char sign = tenths < 0 ? '-' : '+';
  if (tenths < 0) tenths = -tenths;
  snprintf(out, n, "%c%d.%d", sign, (int)(tenths / 10), (int)(tenths % 10));
}

/* Counters are read at a glance, not audited. Past four digits the exact value
   matters far less than the column staying inside its width. */
static void fmtCount(char* out, size_t n, uint32_t v) {
  if (v < 10000) snprintf(out, n, "%u", (unsigned)v);
  else if (v < 1000000) snprintf(out, n, "%uk", (unsigned)(v / 1000));
  else snprintf(out, n, "%uM", (unsigned)(v / 1000000));
}

static void fmtLabel(char* out, size_t n, const NeighbourRow& p) {
  if (p.name != NULL && p.name[0] != 0) {
    strncpy(out, p.name, n - 1);
    out[n - 1] = 0;
    return;
  }
  // No advert heard yet, so all we can honestly show is the path hash.
  size_t j = 0;
  for (uint8_t i = 0; i < p.hash_len && j + 2 < n; i++) {
    snprintf(&out[j], n - j, "%02X", p.hash[i]);
    j += 2;
  }
  out[j] = 0;
}

int NeighboursScreen::rowCapacity(DisplayDriver& display) const {
  int first = TITLE_Y + _pitch * 3;      // name, config, column labels
  int rows = (display.height() - first) / _pitch;
  return rows < 0 ? 0 : rows;
}

int NeighboursScreen::render(DisplayDriver& display) {
  char tmp[NEIGHBOUR_TMP_LEN];
  const int W = display.width();

  display.startFrame();
  display.setTextSize(_text_size);
  display.setColor(UIColor::primary_txt);

  const int cap = rowCapacity(display);
  const int have = _src.numNeighbours();
  const int shown = have < cap ? have : cap;

  /* The overflow badge shares the title line. Working it out first lets the
     title be ellipsized around it, so a long node name can never paint over
     the one piece of text that says the list is incomplete. */
  int title_w = W;
  if (have > shown) {
    snprintf(tmp, sizeof(tmp), "+%d", have - shown);
    display.setColor(UIColor::secondary_txt);
    display.drawTextRightAlign(W, TITLE_Y, tmp);
    display.setColor(UIColor::primary_txt);
    title_w = W - display.getTextWidth(tmp) - display.getTextWidth("  ");
  }

  if (_title != NULL) {
    char title[NEIGHBOUR_TMP_LEN];
    display.translateUTF8ToBlocks(title, _title, sizeof(title));
    display.drawTextEllipsized(0, TITLE_Y, title_w, title);
  }
  if (_subtitle != NULL) {
    display.setColor(UIColor::secondary_txt);
    display.drawTextRightAlign(W, TITLE_Y + _pitch, _subtitle);
    display.setColor(UIColor::primary_txt);
  }

  /* Column geometry, right to left, sized to the data that is ACTUALLY on
     screen rather than to a worst case. Reserving room for "9999" in every
     numeric column costs 19 of the 21 characters a 200px panel gives you at
     the stock proportional font, which leaves nothing for the name. Measuring
     the real values gives that width back on the overwhelmingly common case
     of small counts, and still cannot overflow, because the widest string
     measured IS the widest string drawn. */
  const int gap = display.getTextWidth(" ");
  int w_snr = display.getTextWidth("SNR");
  int w_rx  = display.getTextWidth("RX");
  int w_fwd = display.getTextWidth("FWD");
  for (int i = 0; i < shown; i++) {
    NeighbourRow p;
    if (!_src.getNeighbour(i, p)) break;
    int w;
    fmtSnr(tmp, sizeof(tmp), p.snr4, p.has_snr);
    if ((w = display.getTextWidth(tmp)) > w_snr) w_snr = w;
    fmtCount(tmp, sizeof(tmp), p.rx);
    if ((w = display.getTextWidth(tmp)) > w_rx) w_rx = w;
    fmtCount(tmp, sizeof(tmp), p.fwd);
    if ((w = display.getTextWidth(tmp)) > w_fwd) w_fwd = w;
  }

  const int x_fwd = W;                       // right edge of the FWD column
  const int x_rx  = x_fwd - w_fwd - gap;
  const int x_snr = x_rx  - w_rx  - gap;
  const int name_w = x_snr - w_snr - gap;

  // A hairline rule reads as a divider at a fraction of the height a blank
  // row would cost, and rows are the scarce resource here.
  const int rule_y = TITLE_Y + _pitch * 2 + 2;
  display.fillRect(0, rule_y, W, 1);

  const int hdr_y = rule_y + 3;
  display.setColor(UIColor::secondary_txt);
  display.drawTextLeftAlign(0, hdr_y, "NAME");
  display.drawTextRightAlign(x_snr, hdr_y, "SNR");
  display.drawTextRightAlign(x_rx,  hdr_y, "RX");
  display.drawTextRightAlign(x_fwd, hdr_y, "FWD");
  display.setColor(UIColor::primary_txt);

  int y = TITLE_Y + _pitch * 3;
  for (int i = 0; i < shown; i++, y += _pitch) {
    NeighbourRow p;
    if (!_src.getNeighbour(i, p)) break;

    char raw[NEIGHBOUR_TMP_LEN];
    fmtLabel(raw, sizeof(raw), p);
    display.translateUTF8ToBlocks(tmp, raw, sizeof(tmp));
    display.drawTextEllipsized(0, y, name_w, tmp);

    fmtSnr(tmp, sizeof(tmp), p.snr4, p.has_snr);
    display.drawTextRightAlign(x_snr, y, tmp);
    fmtCount(tmp, sizeof(tmp), p.rx);
    display.drawTextRightAlign(x_rx, y, tmp);
    fmtCount(tmp, sizeof(tmp), p.fwd);
    display.drawTextRightAlign(x_fwd, y, tmp);
  }

  if (have == 0) {
    display.setColor(UIColor::secondary_txt);
    display.drawTextCentered(W / 2, TITLE_Y + _pitch * 4, "no neighbours");
  }

  display.endFrame();
  return _refresh_ms;
}
