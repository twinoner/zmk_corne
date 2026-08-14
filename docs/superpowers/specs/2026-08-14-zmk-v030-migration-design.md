# Migrating zmk_corne off the ds87 fork to ZMK v0.3.0

**Date:** 2026-08-14
**Status:** Approved, ready for implementation planning

## Problem

`config/west.yml` pins ZMK to `daniel2887/zmk@ds87-mouse-pim447`, a fork carrying PIM447
trackball support. That fork is built on **Zephyr v3.0.0+zmk-fixes**, which ships **LVGL 7**.

`M165437/nice-view-gem` targets **LVGL 8+**. It cannot build against the fork at any revision:

- The fork's `app/src/display/Kconfig` selects `LVGL`, `LVGL_THEMES`, `LVGL_THEME_MONO`; its
  widgets call `lv_obj_set_style_local_text_font`, `lv_label_set_align`,
  `lv_obj_create(parent, copy)` — LVGL 7.
- gem's `Kconfig.defconfig` sets `LV_Z_VDB_SIZE`, `LV_Z_BITS_PER_PIXEL`, `LV_COLOR_DEPTH_1`,
  `LV_Z_MEM_POOL_SIZE` and selects `LV_USE_CANVAS` / `LV_USE_ANIMIMG`. Grepping the fork for
  `LV_Z_`, `LV_USE_` or `LV_COLOR_DEPTH` returns nothing — none of those symbols exist there.
- gem calls `lv_animimg_*`, which does not exist in LVGL 7 at all.
- gem includes `zmk/events/endpoint_changed.h`; the fork only has
  `endpoint_selection_changed.h`.

gem's own README states its compatibility matrix: ZMK `v0.3` → gem `v0.3.0`; ZMK `main`
(Zephyr 4.1+) → gem `main`. The fork is older than both — upstream ZMK v0.3.0 is Zephyr 3.5.

The only path to running nice-view-gem is to leave the fork.

## Goal

Move to upstream `zmkfirmware/zmk@v0.3.0`, replacing the fork's PIM447 support with a new
Zephyr `input` driver plus ZMK's native pointing subsystem, then enable nice-view-gem.

## Non-goals

- Moving to ZMK `main` / Zephyr 4.1. That would additionally require the
  `nice_nano_v2/nrf52840/zmk` board variant and the `@main` reusable workflow. Out of scope.
- Porting the fork's acceleration model (inertia, `norm`, `exactness`, `max-accel`). Deferred;
  see "Deferred work".
- PIM447 LED control (registers `0x00`–`0x03`). Already disabled in the current config.

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Pointer feel | Plain scaling first, tune later | Keeps the new driver ~150 LOC instead of ~400 during the riskiest change |
| Scroll keys | Layer-driven via input-listener child node | Preserves both existing semantics with zero custom behavior code |
| Staging | Three stages, each ending in a flashable build | Hardware is the only real feedback loop; isolate failure classes |
| Driver location | In this repo | The repo already declares `zephyr/module.yml`; avoids two-repo version pinning during bring-up |

## Verified facts

Established by reading the cloned repositories, not from memory:

- ZMK v0.3.0 is **hwmv1** (`app/boards/arm/nice_nano/`). The board name stays plain
  `nice_nano_v2` and `.github/workflows/build.yml` stays pinned to `@v0.3.0`. The
  "explicit ZMK compat" gate that breaks `@main` never applies.
- v0.3.0 ships the `corne`, `nice_view` and `nice_view_adapter` shields.
- v0.3.0 provides `app/src/pointing/` with these input processor instances:
  `zip_xy_scaler`, `zip_x_scaler`, `zip_y_scaler`, `zip_scroll_scaler`, `zip_xy_transform`,
  `zip_scroll_transform`, `zip_xy_to_scroll_mapper`, `zip_xy_swap_mapper`, `zip_temp_layer`,
  `zip_button_behaviors`.
- `zmk,input-listener` supports a child binding with `layers`, `input-processors` and
  `process-next` — this is what makes the layer-driven scroll design work.
- `&mkp MB1/MB2/MB3` are unchanged in v0.3.0 (`dt-bindings/zmk/pointing.h`).
- `input_listener.c` maps `INPUT_BTN_0`–`INPUT_BTN_4` to mouse buttons via
  `btn = evt->code - INPUT_BTN_0`, so `INPUT_BTN_0` is left click.
- Upstream's v0.3.0 `corne` shield already defines `kscan0`, `default_transform`,
  `five_column_transform`, `col-offset`, `col-gpios` and the `oled` node — everything the
  local `config/corne.dtsi` duplicates — **plus** `foostan_corne_5col/6col` physical layouts.
- `nice_view_adapter` reassigns `spi0` and sets `&pro_micro_i2c { status = "disabled"; }`,
  because SPIM0 and TWIM0 are the same hardware block on nRF52840. **The trackball must
  therefore stay on `i2c1`.**
- The fork's sensor driver is a thin I²C register reader: regs `0x04` LEFT, `0x05` RIGHT,
  `0x06` UP, `0x07` DOWN, `0x08` SWITCH; `dx = right − left`, `dy = down − up`. All the
  acceleration lives in the separate 437-line `app/src/mouse/trackball_pim447.c` glue.

## Architecture

### Repo shape after the migration

```
zmk_corne/
  zephyr/module.yml          # + build.cmake, build.kconfig, settings.dts_root
  CMakeLists.txt             # add_subdirectory(drivers)
  Kconfig                    # rsource "drivers/Kconfig"
  drivers/
    CMakeLists.txt
    Kconfig
    input/
      CMakeLists.txt
      Kconfig                # config INPUT_PIM447
      input_pim447.c
  dts/bindings/input/
    pimoroni,pim447.yaml
  config/
    layers.h                 # NEW — shared layer defines
    west.yml
    corne.conf
    corne_left.conf
    corne_right.conf
    corne_right.overlay      # deleted in stage 1, reintroduced in stage 2
    corne.keymap
  build.yaml
  .github/workflows/build.yml
```

`config/layers.h` is new and load-bearing. The overlay must name the `SCROLL` layer in the
input-listener's child node, and a `.overlay` cannot see `#define`s from the keymap. Both
files include this header so the layer numbering has exactly one definition.

### Data flow

```
PIM447 (I²C 0x0a on i2c1)
  → input_pim447 driver, polled every poll-interval ms
  → input_report_rel(INPUT_REL_X/Y), input_report_key(INPUT_BTN_0)
  → zmk,input-listener "trackball_listener"
       base:            zip_xy_scaler            → HID mouse move
       when SCROLL on:  zip_xy_to_scroll_mapper
                        + zip_scroll_scaler      → HID scroll
  → HID report to host (right half is central, so no input-split hop)
```

## Stage 1 — base migration

No trackball, no gem. Proves the ZMK bump in isolation.

1. `config/west.yml` — a single `zmk` project from the `zmkfirmware` remote at `v0.3.0`,
   importing `app/west.yml`. Remove the `zmk_ds87` remote. Do **not** add a second project
   named `zmk`; west requires unique project names, and a duplicate is what broke CI run 262.
2. **Delete `config/corne.dtsi`** and **delete `config/corne_right.overlay`**. Both are stale
   copies of the pre-v0.3.0 shield and would collide with upstream's nodes and physical layouts.
3. `build.yaml` and `.github/workflows/build.yml` — unchanged.
4. `config/corne.conf` — `CONFIG_ZMK_MOUSE=y` → `CONFIG_ZMK_POINTING=y`.
5. `config/corne_right.conf` — drop `CONFIG_SENSOR`, `CONFIG_ZMK_TRACKBALL_PIM447`,
   `CONFIG_ZMK_MOUSE`. Keep `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`.
6. `config/layers.h` — define `DEFAULT 0`, `LOWER 1`, `RAISE 2`, `ADJUST 3`, `SCROLL 4`.
7. `config/corne.keymap`:
   - Remove `#include <dt-bindings/zmk/trackball_pim447.h>` and the
     `#ifdef CONFIG_ZMK_TRACKBALL_PIM447` / `PIM447_SCROLL_MOVE` block.
   - `<dt-bindings/zmk/mouse.h>` → `<dt-bindings/zmk/pointing.h>`.
   - Replace the inline layer `#define`s with `#include "layers.h"`.
   - Add a fifth `scroll_layer` (label `"Scroll"`), all `&trans`.
   - In `lower_layer`, `&pim447_scroll_move` → `&mo SCROLL`, `&pim447_toggle` → `&tog SCROLL`.
   - Leave `&mkp MB1/MB3/MB2` as-is.
8. Audit every remaining `CONFIG_` in all three `.conf` files against v0.3.0's Kconfig and
   drop or rename any symbol that no longer exists. Zephyr silently ignores unknown symbols,
   so a stale one shows up as missing behavior rather than a build failure.

**Verification:** all three matrix jobs green; flash both halves; typing, all four layers, the
conditional ADJUST layer, and `&mkp` clicks all work. The trackball is inert at this stage.

## Stage 2 — PIM447 input driver

### Module wiring

`zephyr/module.yml` gains `build.cmake: .`, `build.kconfig: Kconfig`, and
`settings.dts_root: .` alongside the existing `settings.board_root: .`. Root `CMakeLists.txt`
and `Kconfig` descend into `drivers/`.

### Driver

`drivers/input/input_pim447.c`, roughly 150 lines:

- `#define DT_DRV_COMPAT pimoroni_pim447`
- `I2C_DT_SPEC_INST_GET(0)` for bus access — the fork's
  `device_get_binding(DT_INST_BUS_LABEL(0))` no longer exists in Zephyr 3.5.
- A `k_work_delayable` rescheduled every `poll-interval` ms.
- Each poll: burst-read registers `0x04`–`0x08`; `dx = right − left`, `dy = down − up`;
  button state is bit 7 of the switch register.
- Emit `input_report_rel(dev, INPUT_REL_X, dx, false, K_NO_WAIT)`, then the same for
  `INPUT_REL_Y` with `sync = true`. Emit `input_report_key(dev, INPUT_BTN_0, pressed, true,
  K_NO_WAIT)` only when the button state changes.
- Skip reporting entirely when `dx`, `dy` and the button are all unchanged, to avoid waking
  the listener on every poll.
- Registered with `DEVICE_DT_INST_DEFINE(...)` at `POST_KERNEL`,
  `CONFIG_INPUT_INIT_PRIORITY`.

`dts/bindings/input/pimoroni,pim447.yaml`: `compatible: "pimoroni,pim447"`,
`include: [i2c-device.yaml]`, one property `poll-interval` (int, default 50).

`drivers/input/Kconfig`: `config INPUT_PIM447`, `bool`, `depends on I2C && INPUT`.

The old binding's `move-factor-*`, `move-inertia-*`, `invert-*`, `swap-axes`, `norm`,
`exactness`, `max-accel`, `scroll-divisor-*`, `mode`, `button`, `power-layer` and
`idle-timeout` properties are all dropped. Sensitivity and orientation move to input
processors; the rest is deferred.

### Devicetree

`config/corne_right.overlay` returns, containing only additions on top of the shield:

- `&pinctrl` — `i2c1_default` and `i2c1_sleep` groups with
  `NRF_PSEL(TWIM_SDA, 1, 4)` and `NRF_PSEL(TWIM_SCL, 1, 6)`, matching the current
  `sda-pin = <36>` / `scl-pin = <38>` (36 = P1.04, 38 = P1.06). Zephyr 3.5 requires pinctrl;
  the bare `sda-pin`/`scl-pin` properties no longer exist.
- `&i2c1` — `status = "okay"`, the three pinctrl properties, and
  `trackball: trackball@a { compatible = "pimoroni,pim447"; reg = <0xa>; poll-interval = <20>; }`.
- A `trackball_listener` node at the devicetree root:

```dts
/ {
  trackball_listener {
      compatible = "zmk,input-listener";
      device = <&trackball>;
      input-processors = <&zip_xy_scaler 11 10>;

      scroll {
          layers = <SCROLL>;
          input-processors = <&zip_xy_to_scroll_mapper>, <&zip_scroll_scaler 1 3>;
      };
  };
};
```

The initial `zip_xy_scaler 11 10` and `zip_scroll_scaler 1 3` values approximate the fork's
`move-factor-* = 110` and `scroll-divisor-* = 3`; they are a starting point for tuning, not a
faithful reproduction.

`config/corne_right.conf` gains `CONFIG_I2C=y`, `CONFIG_INPUT=y`, `CONFIG_INPUT_PIM447=y`.

No `zmk,input-split` node is needed: the trackball is on the right half, which is already the
central.

**Verification:** CI green; flash the right half; ball moves the cursor; holding `&mo SCROLL`
scrolls; `&tog SCROLL` latches scroll mode; pressing the ball left-clicks. Then tune the
scaler ratios.

## Stage 3 — nice-view-gem

1. `config/west.yml` — add the `m165437` remote (`https://github.com/M165437`) and a
   `nice-view-gem` project at `revision: v0.3.0`, matching the ZMK pin.
2. `build.yaml` — `nice_view` → `nice_view_gem` on both halves.
3. `config/corne.conf` — set `CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y`; remove the built-in
   `CONFIG_ZMK_WIDGET_*` lines, which apply only to the built-in status screen.

**Verification:** CI green; flash both halves; the gem screen renders, with the animation on
the left (peripheral) and status widgets on the right (central).

## Testing approach

This repo has no unit-test harness, and ZMK's `native_posix` suite does not cover out-of-tree
input drivers. Standing one up would cost more than the port itself. Verification for every
stage is therefore **CI compile + flash + manual check on hardware**.

This is a deliberate departure from test-first development and is the main reason for the
three-stage split: each stage changes one class of thing, so a hardware regression points at a
known, small surface.

## Risks

1. **Kconfig drift.** `ZMK_MOUSE` → `ZMK_POINTING` is confirmed; other symbols in the three
   `.conf` files have not each been checked. Unknown symbols fail silently as missing
   behavior. Stage 1 step 8 addresses this.
2. **`i2c1` pin mapping unconfirmed.** The psels were derived from the existing overlay's pin
   numbers, but that P1.04/P1.06 are physically wired to the trackball on this build has not
   been verified against hardware.
3. **Feel regression at stage 2, by design.** Inertia, `norm`, `exactness` and `max-accel` are
   gone until deliberately ported.
4. **Split role asymmetry.** Right is central because the trackball lives there. Nothing in
   this migration changes that, but it is unusual and worth re-checking if pairing misbehaves.

## Deferred work

- Porting the acceleration model into `input_pim447.c` (inertia, `norm` EUCLID/MAX,
  `exactness`, `max-accel`), if plain scaling proves unsatisfying.
- Extracting the driver into a standalone `zmk-pim447-input` module repo, if it turns out to
  be worth sharing.
- PIM447 LED control.
- `zip_temp_layer` for automatic scroll-on-move.
