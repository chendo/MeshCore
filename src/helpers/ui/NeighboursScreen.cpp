#include "NeighboursScreen.h"

#include <stdio.h>
#include <string.h>

/* Four header lines, every one of them a whole pitch apart: our name, the radio
   config, a rule, then the column labels. Row 0 of the table follows one pitch
   below. Everything lands on the same grid on purpose -- the first version
   computed the rule and the labels with their own little offsets, which held
   together at a pitch of 11 and collapsed at 6, drawing the column labels one
   unit from the first peer row. */
static const int TITLE_Y = 2;
static const int HDR_LINES = 4;

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

/* Distance. 0 is the observer's "never placed it" and must not print as a
   distance of nothing; 1 means we hear the node's own radio. */
static void fmtHops(char* out, size_t n, uint8_t hops) {
  if (hops == 0) snprintf(out, n, "?");
  else snprintf(out, n, "%u", (unsigned)hops);
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

int NeighboursScreen::rowCapacity(DisplayDriver& display, int avail_h) const {
  int first = TITLE_Y + _pitch * HDR_LINES;   // name, config, rule, column labels
  int rows = (avail_h - first) / _pitch;
  return rows < 0 ? 0 : rows;
}

int NeighboursScreen::render(DisplayDriver& display) {
  // Standalone: this screen owns the frame and the whole panel.
  display.startFrame();
  display.setTextSize(_text_size);
  int next_ms = renderBody(display, display.height());
  display.endFrame();
  return next_ms;
}

int NeighboursScreen::renderBody(DisplayDriver& display, int avail_h) {
  char tmp[NEIGHBOUR_TMP_LEN];
  const int W = display.width();

  display.setColor(UIColor::primary_txt);

  const int cap = rowCapacity(display, avail_h);
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
  int w_hop = display.getTextWidth("HOP");
  int w_snr = display.getTextWidth("SNR");
  int w_rx  = display.getTextWidth("RX");
  int w_fwd = display.getTextWidth("FWD");
  for (int i = 0; i < shown; i++) {
    NeighbourRow p;
    if (!_src.getNeighbour(i, p)) break;
    int w;
    fmtHops(tmp, sizeof(tmp), p.hops);
    if ((w = display.getTextWidth(tmp)) > w_hop) w_hop = w;
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
  const int x_hop = x_snr - w_snr - gap;
  const int name_w = x_hop - w_hop - gap;

  /* The divider is drawn as TEXT, not with fillRect. In GxEPDDisplay,
     setCursor() adds EINK_Y_OFFSET and a font baseline correction while
     fillRect() adds neither, so the two live in coordinate spaces 15.6px apart:
     a rect placed relative to a text baseline lands on top of the text. Using a
     row of dashes keeps the divider in the same space as everything around it
     and makes the screen immune to that difference on every driver. */
  const int dash_w = display.getTextWidth("-");
  int dashes = dash_w > 0 ? W / dash_w : 0;
  if (dashes > (int)sizeof(tmp) - 1) dashes = (int)sizeof(tmp) - 1;
  for (int i = 0; i < dashes; i++) tmp[i] = '-';
  tmp[dashes > 0 ? dashes : 0] = 0;
  display.setColor(UIColor::secondary_txt);
  display.drawTextLeftAlign(0, TITLE_Y + _pitch * 2, tmp);

  const int hdr_y = TITLE_Y + _pitch * 3;
  display.drawTextLeftAlign(0, hdr_y, "NAME");
  display.drawTextRightAlign(x_hop, hdr_y, "HOP");
  display.drawTextRightAlign(x_snr, hdr_y, "SNR");
  display.drawTextRightAlign(x_rx,  hdr_y, "RX");
  display.drawTextRightAlign(x_fwd, hdr_y, "FWD");
  display.setColor(UIColor::primary_txt);

  int y = TITLE_Y + _pitch * HDR_LINES;
  for (int i = 0; i < shown; i++, y += _pitch) {
    NeighbourRow p;
    if (!_src.getNeighbour(i, p)) break;

    char raw[NEIGHBOUR_TMP_LEN];
    fmtLabel(raw, sizeof(raw), p);
    display.translateUTF8ToBlocks(tmp, raw, sizeof(tmp));
    display.drawTextEllipsized(0, y, name_w, tmp);

    fmtHops(tmp, sizeof(tmp), p.hops);
    display.drawTextRightAlign(x_hop, y, tmp);
    fmtSnr(tmp, sizeof(tmp), p.snr4, p.has_snr);
    display.drawTextRightAlign(x_snr, y, tmp);
    fmtCount(tmp, sizeof(tmp), p.rx);
    display.drawTextRightAlign(x_rx, y, tmp);
    fmtCount(tmp, sizeof(tmp), p.fwd);
    display.drawTextRightAlign(x_fwd, y, tmp);
  }

  if (have == 0) {
    display.setColor(UIColor::secondary_txt);
    display.drawTextCentered(W / 2, TITLE_Y + _pitch * HDR_LINES, "no neighbours");
  }

  return _refresh_ms;
}
