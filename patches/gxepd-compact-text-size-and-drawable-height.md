# `GxEPDDisplay`: a compact text size, and a `height()` that is drawable

**Severity:** low, and additive. Nothing existing changes behaviour: no caller
passes text size 0 today, and no screen paired with this driver reads `height()`.

**Bucket:** 2 (seam). Two changes to `src/helpers/ui/GxEPDDisplay.{h,cpp}`,
about 12 lines. Offer as one upstream PR.

## 1. Text size 0 selects the built-in 5x7 font

`setTextSize()` maps 1/2/3 onto `FreeSans9pt7b`, `FreeSansBold12pt7b` and
`FreeSans18pt7b`, with a `default:` falling back to the first. The smallest of
those has a 22px line advance and a 10px digit, which on the 200x200 panel of a
ThinkNode M5 is **20 columns by 9 lines**.

That is enough for a headline and not enough for a table. A neighbours list, a
sensor readout, a packet log — anything with more than a handful of rows — has
no size to render at. Concretely, measured against the font's own glyph table:

| | FreeSans9pt7b | built-in 5x7 |
|---|---|---|
| advance / line height | 10px / 22px | 6px / 8px |
| 200px panel | 20 cols x 9 rows | **33 cols x 25 rows** |
| `VIC-NorthcoteNW-EDG-01` | 214px, overflows | 132px, fits |

Adafruit_GFX already ships the classic font and `setFont(NULL)` selects it, so
this costs no flash — it is already linked. Measured: `ThinkNode_M5_Repeater`
is byte-identical at 1,125,825 B (85.9%) with and without the change.

```cpp
case 0:  // Compact: the built-in 5x7 font, 6px advance and 8px lines.
  display.setFont(NULL);
  _y_px_adj = -CLASSIC_FONT_BASELINE_PX;
  return;
```

### The baseline wrinkle, which is the only subtle part

Adafruit_GFX puts the cursor on the **baseline** for a custom font and on the
**top-left** for the built-in one. `EINK_Y_OFFSET` (10 units, ~15.6px) exists to
make the baseline convention land on-panel. Selecting the built-in font without
compensating draws every line roughly 13px lower than the same `y` at size 1 —
so a screen that mixes sizes gets a stagger, and the bottom row falls off.

Hence `_y_px_adj`, applied in `setCursor()` and reset to 0 by every other size.
It is held in panel pixels rather than display units because it is a font
metric, not a layout offset. The effect is that `y` means the same thing at
every text size, which is what a caller already assumes.

## 2. `height()` reports the drawable height

The constructor hardcodes `DisplayDriver(128, 128)` while `setCursor()` shifts
every `y` down by `EINK_Y_OFFSET` before it reaches the panel. So the bottom
`EINK_Y_OFFSET` units of the reported space cannot be drawn on. A screen that
sizes itself from `height()` — which is the portable way to write one — runs its
last row off the glass.

`DisplayDriver(128, 128 - (int)(EINK_Y_OFFSET))` makes the report honest. Every
variant that sets the macro sets it to 10 (`lilygo_techo_lite`, `mesh_pocket`,
`thinknode_m5`, `wio-tracker-l1-eink`), and the no-model fallback branch uses 10
too, so this is a uniform 128 -> 118.

Nothing regresses: the only `height()` reader in the tree is
`examples/companion_radio/ui-tiny/UITask.cpp`, and no env pairs `ui-tiny` with
`GxEPDDisplay` (`ui-tiny` is `lilygo_techo_card` only). Existing GxEPD screens
use absolute `y` values and are unaffected.

## Verified

- `ThinkNode_M5_Repeater` (ESP32-S3, upstream env): SUCCESS, 1,125,825 B, 85.9%
  — the same figure as before the change.
- `ThinkNode_M1_repeater` (nRF52840, upstream env): SUCCESS, 303,904 B, 37.3%.
- `test/test_neighbours` pins the resulting geometry: 16 peer rows at pitch 6 on
  the M5's 128x118 unit space, with a full 22-character node name intact.
