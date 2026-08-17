# Trackball acceleration: a `zip_accel` input processor

**Date:** 2026-08-17
**Status:** Approved, ready for implementation planning

## Problem

Commit `6c0790b` ("feat: add previous trackball config") added thirteen properties to the
`trackball@a` node in `config/corne_right.overlay`, copied from the pre-migration ds87 fork
config. CI fails at devicetree parse:

```
devicetree error: /tmp/zmk-config/config/corne_right.overlay:50 (column 11):
  parse error: expected number or parenthesized expression
```

Line 50 is `norm = <PIM447_NORM_MAX>;`. DTS is run through the C preprocessor before dtc sees
it; `PIM447_NORM_MAX` is defined nowhere in this repo, so it survives preprocessing as a bare
identifier where a cell value is required.

The parse error is only the surface failure. `dts/bindings/input/pimoroni,pim447.yaml`
declares one property and `drivers/input/input_pim447.c` reads one:

```c
.poll_interval_ms = DT_INST_PROP(n, poll_interval),   /* the only DT_INST_PROP */
```

All thirteen added properties are inert. Deleting line 50 alone would produce a green build
that changes nothing about how the trackball feels.

### What the fork actually did

The properties were copied from `daniel2887/zmk@ds87-mouse-pim447`. Reading that fork's
`app/src/mouse/trackball_pim447.c` (437 lines) shows `move_inertia_x`, `move_inertia_y`,
`norm`, `exactness` and `max_accel` each appear **exactly once** — on their own `#define`
line, never referenced again. They were dead config in the fork too.

The feel came from two mechanisms whose names appear in no devicetree property:

1. **`ds87_accel()` (line 93)** — a hardcoded 30-entry lookup table indexed by `|delta|`:

   ```c
   {0,3,7,10,48,86,124,162,200,260,319,379,438,498,558,617,677,737,796,856,
    915,975,1035,1094,1154,1213,1273,1333,1392,1452}
   ```

   Expressed as gain, it is an aggressive curve with a sharp knee at index 4:

   | \|delta\| | 1 | 2 | 3 | **4** | 6 | 10 | 20 | 29+ |
   |---|---|---|---|---|---|---|---|---|
   | gain | 3.0x | 3.5x | 3.3x | **12x** | 20.7x | 31.9x | 45.8x | 50.1x |

2. **`filter_delta_history()` (line 118)** — an 8-sample boxcar average over the accelerated
   deltas. This, not any `move-inertia` property, is the smoothing that made the ball feel
   weighted.

The fork polled at the same `poll-interval` (line 284), so its table maps onto the current
20 ms driver one-to-one with no rescaling.

**The regression, quantified.** The fork ran 3x-50x gain. The current firmware runs
`zip_xy_scaler 11 10` — a flat **1.1x**. That is roughly 3x too slow when nudging and 45x too
slow when sweeping. Restoring the properties as written would have fixed none of it.

Two corollaries follow from `ds87_accel(dx)` and `ds87_accel(dy)` being independent calls:
per-axis independence is faithful to the original, and `norm` (a 2D magnitude metric) has no
cross-axis coupling to preserve. `norm` is dropped from this design.

## Goal

A `zip_accel` input processor in this repo that restores the fork's feel by default and
exposes acceleration, inertia and precision as real, per-axis devicetree tuning.

## Non-goals

- **Coasting / momentum.** "Inertia" here means velocity smoothing: the cursor stops when the
  ball stops. Coasting would need a timer emitting synthetic events after input ceases.
- **`norm` / cross-axis coupling.** Dead in the fork, meaningless under per-axis smoothing.
- **Accelerating scroll.** See "Scroll interaction" below.
- **Extracting this into a shareable module repo.** Same deferral as the driver.

## Approach

Three options were considered.

**A. A new input processor in this repo — chosen.** Keeps `input_pim447.c` a pure register
reader, keeps tuning in devicetree, adds no dependency, does not touch `west.yml` and so
leaves the v0.3.0 pin alone. Reusable by any future pointing device.

**B. `oleksandrmaslov/zmk-pointing-acceleration` as a west module.** Cheapest, but it has no
inertia — the specific thing wanted here — its README pins `revision: main` (an unpinned
external dependency, exactly what the pinning discipline exists to prevent), and its v0.3.0
compatibility is unstated.

**C. Port into `input_pim447.c`.** Easiest math, since the driver holds `dx` and `dy` in one
function. Rejected: it re-couples feel to the I2C driver and makes it un-reusable.

Note that the migration spec's "Deferred work" section proposed C ("Porting the acceleration
model into `input_pim447.c`"), which contradicts the standing rule that tuning belongs in
processors and never the driver. **This spec resolves that contradiction in favour of the
processor.** The migration spec's line is superseded.

## Algorithm

Per axis, the processor holds `ema_q8` (Q8 fixed point), `frac_q8` and `last_ms`. For each
matching event with value `v` (already scaled by the upstream `zip_x_scaler` /
`zip_y_scaler`):

1. **Idle reset.** Events fire only on non-zero movement, so a stale velocity would cause a
   jump after a pause. If `k_uptime_get_32()` shows a gap over `ACCEL_IDLE_RESET_MS` (100 ms,
   5 poll intervals), zero `ema_q8` and `frac_q8`.
2. **Curve.** If `|v| <= exactness`, pass through unaccelerated. Otherwise
   `accel = sign(v) * curve[MIN(|v|, 29)] * max_accel / 100`.
3. **Inertia.** `ema_q8 = (ema_q8 * inertia + (accel << 8) * (100 - inertia)) / 100`.
4. **Fraction carry.** `frac_q8 += ema_q8; out = frac_q8 >> 8; frac_q8 -= out << 8`.
   Arithmetic shift gives floor division with a non-negative remainder, so error diffusion is
   consistent for negative values and total displacement is preserved.
5. **Emit.** `event->value = CLAMP(out, INT16_MIN, INT16_MAX)`.

`inertia`, `max_accel` and `exactness` are read from the axis-specific config in every step.

**Why 78 is the inertia default.** An N-tap boxcar is equivalent to an EMA with
`alpha = 2/(N+1)`. For the fork's 8-tap filter that is 0.22, so `inertia = 78`. The value
`70` in the old config is close, which is reassuring but coincidental — the property was dead.

**Overflow.** Worst case is `curve[29] = 1452` with `max-accel = 1000`, giving
`14520 << 8 = 3717120`, times 99 = ~3.7e8. Comfortably inside `int32_t`. Zephyr binding YAML
cannot express numeric ranges, so the ceiling is enforced by `BUILD_ASSERT` in the instance
macro rather than by the binding.

## Devicetree interface

```dts
/omit-if-no-ref/ zip_accel: zip_accel {
    compatible = "zmk,input-processor-accel";
    #input-processor-cells = <0>;
    type = <INPUT_EV_REL>;
    x-code = <INPUT_REL_X>;
    y-code = <INPUT_REL_Y>;
};
```

The axes are named separately rather than given as one `codes` list, because per-axis
tuning has to tell them apart when dispatching an event.

Used in `config/corne_right.overlay` as:

```dts
input-processors = <&zip_x_scaler 11 10>, <&zip_y_scaler 11 10>, <&zip_accel>;
```

The scalers run first so `move-factor` is applied before the curve is indexed — the fork's
order (`dx = raw*FACTOR/100`, then `ds87_accel(dx)`). Splitting `zip_xy_scaler` into the
per-axis `zip_x_scaler` and `zip_y_scaler` (both shipped by ZMK v0.3.0) is what makes
`move-factor` per-axis; it needs no new code, and each gets its own `track-remainders` slot.

The node above carries the defaults. Tuning is done by overriding it in
`config/corne_right.overlay`, which is where all trackball feel already lives:

```dts
&zip_accel {
    max-accel-x = <130>;
    max-accel-y = <130>;
};
```

All tuning is per axis:

| Property | Default | Meaning |
|---|---|---|
| `max-accel-x` / `max-accel-y` | `100` | Scales the curve. `100` reproduces the fork's gain curve exactly. |
| `move-inertia-x` / `move-inertia-y` | `78` | EMA smoothing, 0-99. `78` matches the 8-tap boxcar. |
| `exactness-x` / `exactness-y` | `0` | `\|v\|` at or below this bypasses acceleration entirely. |
| `accel-curve` | ds87 table | Optional override, shared by both axes. Must be exactly 30 entries, and index 0 must stay `0` so zero movement stays zero. Zephyr bindings cannot constrain array length, so `BUILD_ASSERT` on `DT_INST_PROP_LEN` enforces it at compile time. |

Every default reproduces the old firmware's gain curve and smoothing characteristic, so the
first flash is a known-good baseline and each knob is a deliberate step away from it.
`exactness` becomes meaningful for the first time: the table's 3x floor denies fine control,
and raising `exactness` buys it back. See Risks for a measured divergence in how smoothing
tails off at the end of a stroke.

### Scroll interaction

`input_listener.c:223` returns early when a layer override matches and `process_next` is
unset, so on the `SCROLL` layer only the child's `zip_xy_to_scroll_mapper` and
`zip_scroll_scaler` run. The base chain — both scalers and `zip_accel` — is skipped, so
acceleration never touches scrolling. The processor's per-axis state goes stale while
scrolling, and the idle reset in step 1 clears it on release.

## File layout

Out-of-tree input processors compile **into the `app` target**, not into a separate Zephyr
library:

```cmake
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_ACCEL app PRIVATE input_processor_accel.c)
```

This is what makes `#include <drivers/input_processor.h>` resolve: the header lives in
`zmk/app/include`, which `app/CMakeLists.txt:25` marks `PRIVATE` to the `app` target, so a
`zephyr_library` cannot see it. Verified against `badjeff/zmk-input-processor-xyz`, which
does exactly this.

Note this is a *different* mechanism from the existing `drivers/input/CMakeLists.txt`, which
uses `zephyr_library_amend()` to extend Zephyr's `input` library. Both mechanisms coexist in
this module; the reason belongs in a comment.

| File | Change |
|---|---|
| `src/pointing/input_processor_accel.c` | new — the processor |
| `src/pointing/accel_math.h` | new — the pure-integer math, host-testable |
| `src/pointing/CMakeLists.txt` | new — the `target_sources_ifdef` above |
| `src/pointing/Kconfig` | new — `if ZMK_POINTING`, `depends on DT_HAS_ZMK_INPUT_PROCESSOR_ACCEL_ENABLED` |
| `dts/bindings/input_processors/zmk,input-processor-accel.yaml` | new — binding |
| `dts/input/processors/accel.dtsi` | new — the `/omit-if-no-ref/` node |
| `CMakeLists.txt` | add `add_subdirectory(src/pointing)` |
| `Kconfig` | add `rsource "src/pointing/Kconfig"` |
| `config/corne_right.overlay` | revert `6c0790b`; include the dtsi; new processor chain |
| `tests/accel/` | new — host-compiled math tests |

`zephyr/module.yml` already sets `dts_root: .`, which puts `dts/bindings` and `dts/` on the
right paths — the CI log confirms `zmk_corne/dts/bindings` reaches `gen_defines.py`. No
manifest changes; the v0.3.0 pin is untouched.

The `zmk,` vendor prefix is used to match ZMK convention (`dts/bindings/vendor-prefixes.txt`
declares only `pimoroni`; `zmk` comes from ZMK's own prefix file, which the build passes).

The binding does `include: ip_zero_param.yaml` to pick up the required
`#input-processor-cells = <0>` constraint and the `track-remainders` property. That file lives
in ZMK's `app/dts/bindings/input_processors/`, not in this repo — it resolves because Zephyr
searches every bindings directory, and the CI log confirms `zmk/app/dts/bindings` is one of
them. This is a cross-module dependency on a ZMK-internal file; the v0.3.0 pin bounds it.

## Testing

`accel_math.h` holds the curve, EMA and fraction carry with no Zephyr dependency, so it
compiles natively. Host tests assert:

- curve lookup at the knee and at both ends, including the clamp — the fork's
  `if (abs > pow_sz)` reads `pow[30]` on a 30-element array, an off-by-one this port fixes;
- `exactness` bypass at and just past the threshold;
- EMA settling to its input under a constant stream;
- fraction carry preserving total displacement across a burst, for negative values too;
- idle reset zeroing state past the threshold.

Firmware verification is CI going green, then a flash-and-feel check on hardware.

## Risks

1. **Defaults are a reconstruction, not a recording.** They reproduce the fork's *code*, but
   the fork's poll loop and HID path differ from ZMK's pointing subsystem. Expect to tune.
2. **`int16_t` event values.** At the default `max-accel = 100` the curve tops out at 1452,
   well inside range; the clamp in step 5 only engages above `max-accel` of roughly 2255. It
   is defensive, not load-bearing, and a silent cap is the right failure mode there.
3. **Processor state is per node, not per listener.** Referencing `zip_accel` from two
   listeners would share state. Single-trackball config, so not a live concern; the binding
   should document it.
4. **`target_sources_ifdef(... app ...)` is an out-of-tree convention, not a documented API.**
   It is what community modules use, but ZMK could restructure it in a future release. The
   v0.3.0 pin bounds this risk.

### Smoothing tail is cut short at the end of a stroke

The fork's 8-tap boxcar ran in an unconditional poll loop, so its filter tail flushed over
subsequent polls even after the ball stopped moving. `drivers/input/input_pim447.c` stays
silent when both deltas are zero, so `zip_accel` stops being called the instant the ball
stops, and the EMA's residual — roughly 3.5 samples of velocity at `inertia = 78` — is
discarded by the idle reset instead of draining out as trailing counts.

Measured at `inertia = 78`, constant `v = 10`: a 5-poll flick emits 790 counts against 1595
unsmoothed (50% short); 10 polls is 33% short; 50 polls is 7% short.

Short, deliberate strokes lose the most, proportionally, since the discarded tail is a fixed
number of samples regardless of stroke length. The tuning lever for this is lowering
`move-inertia-*`, not raising `max-accel-*` — inertia controls how much history (and thus how
much residual) the EMA carries, while max-accel only scales the curve.

A tail flush (continuing to emit decaying events for a few polls after input stops) was
deliberately not implemented: it would mean emitting movement after the ball is at rest, and
coasting after input stops is exactly the behavior the design's Non-goals section rules out.

## Deferred work

- Promoting `ACCEL_IDLE_RESET_MS` to a devicetree property, if the 100 ms default proves wrong.
- Per-axis `accel-curve` override, if one shared table proves limiting.
- Re-introducing `norm` and cross-axis coupling, if per-axis independence feels wrong on
  diagonals.
- Extracting the processor and driver into a standalone module repo.
