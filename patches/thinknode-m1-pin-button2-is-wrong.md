# ThinkNode M1: `PIN_BUTTON2` is pin 11, but the button is on P1.07 (39)

**Severity:** medium. The M1's second button is unusable in every upstream
build. Nothing crashes; the button simply never responds, and there is no way
to tell that from the firmware side because a pin with nothing on it and a
button that is never pressed look identical.

**Bucket:** upstream bug in `variants/thinknode_m1/variant.h`. One line.

## What is wrong

```c
#define PIN_BUTTON2             (11)
#define BUTTON_PIN2             PIN_BUTTON2
```

Elecrow's own datasheet for the board gives a different pin. From
`Datasheet/ThinkNode M1_LoRa_Meshtastic_Transceiver_DataSheet.pdf` in
`Elecrow-RD/ThinkNode-M1-LoRa-Meshtastic-Transceiver-...-nRF52840`:

| Control | Signal | Pin | Documented behaviour |
|---|---|---|---|
| Function Button | `FK BUTTON_TOUCH` | **P1.07** | single: send a location ping · double: toggle GPS · long: sleep |
| Page Turn Button | `TK BUTTON` | **P1.10** | single: switch pages |
| Reset Button | `RST MCU_RST` | P0.18/NRESET | reset |

P1.07 is GPIO **39**; P1.10 is GPIO **42**. `PIN_USER_BTN` is already 42 and
works. `PIN_BUTTON2` should be **39**, not 11.

## How it was found

A serial logger recorded every `MomentaryButton` event and every raw pin edge
across three full press tests on real hardware:

- pin 42 — `CLICK`, `DOUBLE`, `LONG` all fire
- pin 11 — **zero edges**, as `INPUT` and as `INPUT_PULLUP`
- pin 39 — `CLICK`, `DOUBLE`, `LONG` all fire, once pointed there

Nothing upstream reads `PIN_BUTTON2` on this board, which is presumably why the
wrong value has survived: the only code that touches it is guarded by
`BACKLIGHT_BTN`, which the M1 does not define.

## Fix

```c
#define PIN_BUTTON2             (39)    // P1.07, "FK BUTTON_TOUCH" per datasheet
```

We do not patch it here -- `variants/companion_m1/platformio.ini` defines its
own `UI_BUTTON2_PIN=39` instead, because `-D PIN_BUTTON2=39` would collide with
the variant's `#define`. That workaround should go away once this lands.

## A second trap in the same area, not a bug

`variants/thinknode_m1/target.cpp` declares the user button as

```c
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);   // pulldownup omitted = false
```

i.e. `pinMode(42, INPUT)`, relying on the board's external pull-up. Building a
*second* `MomentaryButton` on the same pin with `pulldownup=true` gives
`INPUT_PULLUP`, and that internal pull-up holds the pin high through a press --
the button then reads as never pressed. Downstream code should use the
variant's `user_btn` rather than declaring its own.
