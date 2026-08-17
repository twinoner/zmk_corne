# Trackball Acceleration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `zip_accel` input processor that restores the ds87 fork's trackball feel by default and exposes acceleration, inertia and precision as per-axis devicetree tuning.

**Architecture:** A header-only, Zephyr-free math module (`src/pointing/accel_math.h`) holds the gain curve, EMA smoothing and fraction carry, so it compiles both into the firmware and natively under `tests/accel`. A thin Zephyr processor (`src/pointing/input_processor_accel.c`) maps devicetree config and per-device state onto it. The processor compiles into ZMK's `app` target rather than a Zephyr library, which is what makes `<drivers/input_processor.h>` resolve.

**Tech Stack:** C11, Zephyr 3.5 / ZMK v0.3.0 input-processor API, devicetree bindings, Kconfig, host `cc` for tests.

**Spec:** `docs/superpowers/specs/2026-08-17-trackball-acceleration-design.md`

## Global Constraints

- **ZMK stays pinned to `zmkfirmware/zmk@v0.3.0`.** No task touches `config/west.yml` or the build workflow.
- **No tuning in `drivers/input/input_pim447.c`.** It stays a pure register reader; all feel lives in devicetree.
- **`src/pointing/accel_math.h` must have zero Zephyr dependencies** — `<stdint.h>` and `<stdbool.h>` only. It is compiled natively by the tests.
- **Every devicetree default reproduces the ds87 fork exactly**: `max-accel-* = 100`, `move-inertia-* = 78`, `exactness-* = 0`, built-in 30-entry curve.
- **`config/corne_right.overlay` must keep both includes**: `"layers.h"` (quoted — the config dir is not on the DTS include path) and `<input/processors.dtsi>` (angle — the `zip_*` labels are `/omit-if-no-ref/`).
- **The trackball stays on `i2c1`.** `nice_view_adapter` reassigns `spi0` and disables `pro_micro_i2c`.
- **SPDX header on every new file:** `SPDX-License-Identifier: MIT`.
- **Curve values, verbatim** (from `daniel2887/zmk@ds87-mouse-pim447`, `app/src/mouse/trackball_pim447.c:94`):
  `0, 3, 7, 10, 48, 86, 124, 162, 200, 260, 319, 379, 438, 498, 558, 617, 677, 737, 796, 856, 915, 975, 1035, 1094, 1154, 1213, 1273, 1333, 1392, 1452`

## File Structure

| File | Responsibility |
|---|---|
| `src/pointing/accel_math.h` | All arithmetic: curve lookup, exactness bypass, EMA, fraction carry, clamp. Header-only, `static inline`, no Zephyr. |
| `src/pointing/input_processor_accel.c` | Zephyr glue only: devicetree config, per-device state, event dispatch to the header. |
| `src/pointing/CMakeLists.txt` | Compiles the processor into the `app` target. |
| `src/pointing/Kconfig` | `ZMK_INPUT_PROCESSOR_ACCEL` and its device bound. |
| `dts/bindings/input_processors/zmk,input-processor-accel.yaml` | The public tuning interface. |
| `dts/input/processors/accel.dtsi` | The `/omit-if-no-ref/ zip_accel` node carrying defaults. |
| `tests/accel/test_accel_math.c` | Host tests for every branch of the math. |
| `tests/accel/run.sh` | Compiles and runs them with `cc`. |

---

### Task 1: Unbreak the devicetree

Commit `6c0790b` added thirteen properties the binding does not declare and the driver never reads. One of them, `norm = <PIM447_NORM_MAX>`, references an undefined macro and fails the DTS parse. Remove the block; the two settings that mattered are already expressed as input processors.

**Files:**
- Modify: `config/corne_right.overlay:37-56`

**Interfaces:**
- Consumes: nothing.
- Produces: a parseable overlay. Task 4 edits this same file again.

- [ ] **Step 1: Confirm the failure reproduces**

Run: `git log --oneline -1` and confirm `6c0790b`. Read `config/corne_right.overlay:50`.
Expected: the line reads `norm = <PIM447_NORM_MAX>;`, and `grep -rn "PIM447_NORM" --exclude-dir=.git .` returns only that one line — the macro is defined nowhere.

- [ ] **Step 2: Remove the dead properties**

Replace lines **37-56** of `config/corne_right.overlay` — from `poll-interval = <20>;` (line 37) through the trackball node's closing `};` (line 56), inclusive. Line 55 is `button = <0>;`; do not stop there or the closing brace will be duplicated. Replace with:

```dts
		/*
		 * poll-interval is the only property this driver reads. The ds87
		 * fork's tuning properties are not declared by our binding and were
		 * dead in the fork itself; feel lives in the input processors below.
		 */
		poll-interval = <20>;
	};
```

- [ ] **Step 3: Verify the file parses as devicetree**

Run: `grep -n "norm\|move-factor\|move-inertia\|max-accel\|exactness\|scroll-divisor\|power-layer" config/corne_right.overlay`
Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add config/corne_right.overlay
git commit -m "fix: drop dead PIM447 tuning properties that broke the DTS parse

The ds87 fork's move-factor/inertia/norm/exactness/max-accel properties are
not declared by dts/bindings/input/pimoroni,pim447.yaml and were never read
by the driver. norm referenced PIM447_NORM_MAX, which is defined nowhere, so
the C preprocessor left a bare identifier where dtc wanted a cell value."
```

---

### Task 2: The gain curve

Port `ds87_accel()` as a pure function, fixing its off-by-one, and add the `exactness` bypass the fork declared but never implemented.

**Files:**
- Create: `src/pointing/accel_math.h`
- Create: `tests/accel/test_accel_math.c`
- Create: `tests/accel/run.sh`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `#define ZMK_ACCEL_CURVE_LEN 30`
  - `static const uint16_t zmk_accel_default_curve[ZMK_ACCEL_CURVE_LEN]`
  - `struct zmk_accel_axis_config { uint16_t max_accel; uint16_t exactness; uint8_t inertia; }`
  - `static inline int32_t zmk_accel_curve_lookup(const uint16_t *curve, const struct zmk_accel_axis_config *cfg, int32_t value)`

- [ ] **Step 1: Write the failing test**

Create `tests/accel/test_accel_math.c`:

```c
/*
 * Host tests for the zip_accel math. No Zephyr: compiled with plain cc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "accel_math.h"

static int failures;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_curve_lookup(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 0, .inertia = 0};
    const uint16_t *c = zmk_accel_default_curve;

    /* Zero stays zero, or the cursor drifts. */
    CHECK(zmk_accel_curve_lookup(c, &cfg, 0) == 0);

    /* Table values, straight off the fork. */
    CHECK(zmk_accel_curve_lookup(c, &cfg, 1) == 3);
    CHECK(zmk_accel_curve_lookup(c, &cfg, 3) == 10);
    CHECK(zmk_accel_curve_lookup(c, &cfg, 4) == 48);
    CHECK(zmk_accel_curve_lookup(c, &cfg, 29) == 1452);

    /* Sign is preserved, magnitude is symmetric. */
    CHECK(zmk_accel_curve_lookup(c, &cfg, -4) == -48);
    CHECK(zmk_accel_curve_lookup(c, &cfg, -29) == -1452);

    /* The fork read pow[30] on a 30-element array. We clamp instead. */
    CHECK(zmk_accel_curve_lookup(c, &cfg, 30) == 1452);
    CHECK(zmk_accel_curve_lookup(c, &cfg, 1000) == 1452);
    CHECK(zmk_accel_curve_lookup(c, &cfg, -1000) == -1452);
}

static void test_curve_max_accel(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 130, .exactness = 0, .inertia = 0};
    const uint16_t *c = zmk_accel_default_curve;

    CHECK(zmk_accel_curve_lookup(c, &cfg, 4) == 62);   /* 48 * 130 / 100 */
    CHECK(zmk_accel_curve_lookup(c, &cfg, -4) == -62);

    const struct zmk_accel_axis_config half = {.max_accel = 50, .exactness = 0, .inertia = 0};
    CHECK(zmk_accel_curve_lookup(c, &half, 4) == 24);  /* 48 * 50 / 100 */
}

static void test_curve_exactness_bypass(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 3, .inertia = 0};
    const uint16_t *c = zmk_accel_default_curve;

    /* At or below the threshold, movement passes through untouched. */
    CHECK(zmk_accel_curve_lookup(c, &cfg, 1) == 1);
    CHECK(zmk_accel_curve_lookup(c, &cfg, 3) == 3);
    CHECK(zmk_accel_curve_lookup(c, &cfg, -3) == -3);

    /* Just past it, the curve takes over. */
    CHECK(zmk_accel_curve_lookup(c, &cfg, 4) == 48);
}

int main(void) {
    test_curve_lookup();
    test_curve_max_accel();
    test_curve_exactness_bypass();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("all checks passed\n");
    return 0;
}
```

Create `tests/accel/run.sh`:

```sh
#!/bin/sh
# Compile and run the zip_accel math tests on the host.
#
# SPDX-License-Identifier: MIT

set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="${TMPDIR:-/tmp}/test_accel_math"

cc -std=c11 -Wall -Wextra -I "$here/../../src/pointing" -o "$out" "$here/test_accel_math.c"
"$out"
```

- [ ] **Step 2: Run test to verify it fails**

```bash
chmod +x tests/accel/run.sh
./tests/accel/run.sh
```

Expected: FAIL — `fatal error: 'accel_math.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `src/pointing/accel_math.h`:

```c
/*
 * Pure-integer acceleration math for the zip_accel input processor.
 *
 * No Zephyr dependencies: this header is compiled both into the firmware and
 * natively by tests/accel. Keep it that way.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZMK_ACCEL_CURVE_LEN 30

/*
 * Gain curve ported verbatim from daniel2887/zmk@ds87-mouse-pim447,
 * app/src/mouse/trackball_pim447.c:94 (ds87_accel). Indexed by the absolute
 * event value; index 0 must stay 0 so zero movement stays zero.
 */
static const uint16_t zmk_accel_default_curve[ZMK_ACCEL_CURVE_LEN] = {
    0,   3,   7,    10,   48,   86,   124,  162,  200,  260,
    319, 379, 438,  498,  558,  617,  677,  737,  796,  856,
    915, 975, 1035, 1094, 1154, 1213, 1273, 1333, 1392, 1452,
};

struct zmk_accel_axis_config {
    uint16_t max_accel; /* percent; 100 leaves the curve as-is */
    uint16_t exactness; /* |value| at or below this bypasses acceleration */
    uint8_t inertia;    /* 0-99; EMA weight given to history */
};

static inline int32_t zmk_accel_curve_lookup(const uint16_t *curve,
                                             const struct zmk_accel_axis_config *cfg,
                                             int32_t value) {
    if (value == 0) {
        return 0;
    }

    int32_t sign = value < 0 ? -1 : 1;
    int32_t mag = value < 0 ? -value : value;

    if (mag <= (int32_t)cfg->exactness) {
        return value;
    }

    /* The fork tested `abs > pow_sz`, which let abs == 30 read one past the
     * end of a 30-element array. Clamp to the last index instead. */
    if (mag >= ZMK_ACCEL_CURVE_LEN) {
        mag = ZMK_ACCEL_CURVE_LEN - 1;
    }

    return sign * (((int32_t)curve[mag] * (int32_t)cfg->max_accel) / 100);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./tests/accel/run.sh`
Expected: PASS — `all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add src/pointing/accel_math.h tests/accel/test_accel_math.c tests/accel/run.sh
git commit -m "feat: port the ds87 acceleration curve as testable host code

Adds the 30-entry gain table with the fork's off-by-one fixed, plus the
exactness bypass the fork declared but never implemented."
```

---

### Task 3: Inertia, fraction carry and idle reset

Wrap the curve in the smoothing pipeline. The EMA replaces the fork's 8-sample boxcar; an 8-tap boxcar equals an EMA with `alpha = 2/(N+1) = 0.22`, hence the `78` default. Fraction carry stops slow movement being truncated to nothing, and the idle reset stops a pause leaving a stale velocity to jump from.

**Files:**
- Modify: `src/pointing/accel_math.h`
- Modify: `tests/accel/test_accel_math.c`

**Interfaces:**
- Consumes: `zmk_accel_curve_lookup`, `struct zmk_accel_axis_config`, `zmk_accel_default_curve` from Task 2.
- Produces:
  - `#define ZMK_ACCEL_IDLE_RESET_MS 100`
  - `struct zmk_accel_axis_state { int32_t ema_q8; int32_t frac_q8; uint32_t last_ms; bool seen; }`
  - `static inline int16_t zmk_accel_apply(const uint16_t *curve, const struct zmk_accel_axis_config *cfg, struct zmk_accel_axis_state *state, int32_t value, uint32_t now_ms)`

- [ ] **Step 1: Write the failing test**

Add to `tests/accel/test_accel_math.c`, above `main`:

```c
/* Feed a constant value at 20 ms intervals and total what comes out. */
static int32_t drive(struct zmk_accel_axis_state *state,
                     const struct zmk_accel_axis_config *cfg, int32_t value, int steps,
                     uint32_t start_ms) {
    int32_t total = 0;
    for (int i = 0; i < steps; i++) {
        total += zmk_accel_apply(zmk_accel_default_curve, cfg, state, value, start_ms + i * 20);
    }
    return total;
}

static void test_no_inertia_passes_curve_through(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 0, .inertia = 0};
    struct zmk_accel_axis_state state = {0};

    /* inertia 0 means no smoothing: out == the curve value, exactly. */
    CHECK(zmk_accel_apply(zmk_accel_default_curve, &cfg, &state, 4, 1000) == 48);
    CHECK(zmk_accel_apply(zmk_accel_default_curve, &cfg, &state, -4, 1020) == -48);
}

static void test_inertia_settles_to_curve_value(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 0, .inertia = 78};
    struct zmk_accel_axis_state state = {0};

    /* Ramps in rather than jumping... */
    int32_t first = zmk_accel_apply(zmk_accel_default_curve, &cfg, &state, 4, 1000);
    CHECK(first > 0);
    CHECK(first < 48);

    /* ...and converges on the unsmoothed value. Integer division truncates,
     * which parks the EMA a few Q8 counts under its true fixed point of
     * 48 << 8, so check the steady-state average rather than one sample.
     * 100 settled steps of 48 is 4800; the truncation costs under 0.5%. */
    drive(&state, &cfg, 4, 200, 1020);                   /* last event at 5000 */
    int32_t settled = drive(&state, &cfg, 4, 100, 5020); /* contiguous, no idle reset */
    CHECK(settled >= 4780);
    CHECK(settled <= 4800);
}

static void test_fraction_carry_preserves_displacement(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 0, .inertia = 78};

    /* A value of 1 maps to a gain of 3, so 500 steps is about 1500 counts —
     * less a ramp-in deficit (tau = 78/22 = 3.5 steps) and a little EMA
     * truncation. The point of the assertion is the floor: without fraction
     * carry each step would emit floor(2.99) = 2 and the total would be
     * around 1000, nowhere near the bound. */
    struct zmk_accel_axis_state pos = {0};
    int32_t total_pos = drive(&pos, &cfg, 1, 500, 1000);
    CHECK(total_pos >= 1450);
    CHECK(total_pos <= 1500);

    /* Negative values use arithmetic shift, which floors rather than
     * truncating toward zero. The carry must compensate so the two directions
     * stay within a count or two of each other. */
    struct zmk_accel_axis_state neg = {0};
    int32_t total_neg = drive(&neg, &cfg, -1, 500, 1000);
    int32_t diff = total_pos + total_neg;
    CHECK(diff <= 2 && diff >= -2);
}

static void test_idle_reset_clears_stale_velocity(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 0, .inertia = 78};
    struct zmk_accel_axis_state state = {0};

    /* Build up a fast sweep. */
    drive(&state, &cfg, 10, 100, 1000);

    /* A short gap keeps the built-up velocity: the next event rides it. */
    struct zmk_accel_axis_state warm = state;
    CHECK(zmk_accel_apply(zmk_accel_default_curve, &cfg, &warm, 10, 3000 + 40) > 200);

    /* A long gap discards it, so the next event starts from rest.
     * ema = (319 << 8) * 22 / 100 = 17966; 17966 >> 8 = 70. */
    struct zmk_accel_axis_state cold = state;
    CHECK(zmk_accel_apply(zmk_accel_default_curve, &cfg, &cold, 10, 3000 + 500) == 70);
}

static void test_first_event_starts_from_rest(void) {
    const struct zmk_accel_axis_config cfg = {.max_accel = 100, .exactness = 0, .inertia = 78};
    struct zmk_accel_axis_state state = {0};

    /* A zeroed state with last_ms == 0 must not be mistaken for a warm one,
     * whatever the uptime happens to be on the first event. */
    CHECK(zmk_accel_apply(zmk_accel_default_curve, &cfg, &state, 10, 5) == 70);
}
```

Add these calls to `main`, before the `failures` check:

```c
    test_no_inertia_passes_curve_through();
    test_inertia_settles_to_curve_value();
    test_fraction_carry_preserves_displacement();
    test_idle_reset_clears_stale_velocity();
    test_first_event_starts_from_rest();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tests/accel/run.sh`
Expected: FAIL — `implicit declaration of function 'zmk_accel_apply'` and `unknown type name 'struct zmk_accel_axis_state'`.

- [ ] **Step 3: Write minimal implementation**

Append to `src/pointing/accel_math.h`, after `zmk_accel_curve_lookup`:

```c
/*
 * Movement gaps longer than this zero the smoothing state. Events only fire on
 * non-zero movement, so without it a pause would leave a stale velocity for the
 * next flick to jump from. Five times the 20 ms poll interval.
 */
#define ZMK_ACCEL_IDLE_RESET_MS 100

struct zmk_accel_axis_state {
    int32_t ema_q8;  /* smoothed output, Q8 fixed point */
    int32_t frac_q8; /* error-diffusion accumulator, Q8 */
    uint32_t last_ms;
    bool seen; /* distinguishes "never used" from "last used at t=0" */
};

static inline int16_t zmk_accel_apply(const uint16_t *curve,
                                      const struct zmk_accel_axis_config *cfg,
                                      struct zmk_accel_axis_state *state, int32_t value,
                                      uint32_t now_ms) {
    if (!state->seen || (now_ms - state->last_ms) > ZMK_ACCEL_IDLE_RESET_MS) {
        state->ema_q8 = 0;
        state->frac_q8 = 0;
    }

    state->last_ms = now_ms;
    state->seen = true;

    int32_t accel = zmk_accel_curve_lookup(curve, cfg, value);

    int32_t inertia = cfg->inertia > 99 ? 99 : (int32_t)cfg->inertia;
    state->ema_q8 = (state->ema_q8 * inertia + (accel << 8) * (100 - inertia)) / 100;

    /* Emit the integer part and keep the fraction, so slow movement
     * accumulates into real counts instead of truncating to nothing.
     * Arithmetic shift floors, which leaves frac_q8 in [0, 255] for both
     * signs and keeps the diffusion consistent. */
    state->frac_q8 += state->ema_q8;
    int32_t out = state->frac_q8 >> 8;
    state->frac_q8 -= out << 8;

    if (out > INT16_MAX) {
        return INT16_MAX;
    }
    if (out < INT16_MIN) {
        return INT16_MIN;
    }

    return (int16_t)out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./tests/accel/run.sh`
Expected: PASS — `all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add src/pointing/accel_math.h tests/accel/test_accel_math.c
git commit -m "feat: add inertia smoothing, fraction carry and idle reset

EMA smoothing replaces the fork's 8-sample boxcar; an 8-tap boxcar equals an
EMA with alpha = 2/(N+1) = 0.22, so 78 is the faithful default."
```

---

### Task 4: The Zephyr processor, wired up

The binding, the node and the C glue only become testable once something references them — an unreferenced `/omit-if-no-ref/` node is dropped, which compiles the driver out entirely. So the overlay change ships with them.

**Files:**
- Create: `dts/bindings/input_processors/zmk,input-processor-accel.yaml`
- Create: `dts/input/processors/accel.dtsi`
- Create: `src/pointing/input_processor_accel.c`
- Create: `src/pointing/CMakeLists.txt`
- Create: `src/pointing/Kconfig`
- Modify: `CMakeLists.txt`
- Modify: `Kconfig`
- Modify: `config/corne_right.overlay`

**Interfaces:**
- Consumes: everything from `accel_math.h` (Tasks 2 and 3).
- Produces: the `zip_accel` devicetree label, referenced from `trackball_listener`.

- [ ] **Step 1: Write the binding**

Create `dts/bindings/input_processors/zmk,input-processor-accel.yaml`:

```yaml
# SPDX-License-Identifier: MIT

description: |
  Velocity-dependent acceleration for relative pointing devices.

  All tuning is per axis. Every default reproduces the ds87 fork's feel
  exactly, so overriding nothing is the known-good baseline.

  Smoothing state is kept per node, not per listener: referencing one instance
  from two input listeners makes them share state.

compatible: "zmk,input-processor-accel"

include: ip_zero_param.yaml

properties:
  type:
    type: int
    required: true
    description: Event type to process, e.g. INPUT_EV_REL.

  x-code:
    type: int
    required: true
    description: Event code treated as the X axis, e.g. INPUT_REL_X.

  y-code:
    type: int
    required: true
    description: Event code treated as the Y axis, e.g. INPUT_REL_Y.

  max-accel-x:
    type: int
    default: 100
    description: |
      Percentage scaling of the gain curve on X. 100 leaves the curve as-is.
      Capped at 2255 by a BUILD_ASSERT, above which the curve saturates the
      int16 event value.

  max-accel-y:
    type: int
    default: 100
    description: Percentage scaling of the gain curve on Y. See max-accel-x.

  move-inertia-x:
    type: int
    default: 78
    description: |
      Weight given to history by the exponential moving average on X, 0-99.
      0 disables smoothing. 78 matches the 8-sample boxcar the fork used.

  move-inertia-y:
    type: int
    default: 78
    description: EMA weight given to history on Y, 0-99. See move-inertia-x.

  exactness-x:
    type: int
    default: 0
    description: |
      Absolute X values at or below this bypass acceleration entirely, buying
      back fine control that the curve's 3x floor otherwise denies.

  exactness-y:
    type: int
    default: 0
    description: Absolute Y values at or below this bypass acceleration.

  accel-curve:
    type: array
    description: |
      Optional gain curve override, shared by both axes and indexed by the
      absolute event value. Must have exactly 30 entries and index 0 must be
      0; both are enforced by BUILD_ASSERT. Omit to use the built-in table.
```

- [ ] **Step 2: Write the node**

Create `dts/input/processors/accel.dtsi`:

```dts
/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/dt-bindings/input/input-event-codes.h>

/ {
	/omit-if-no-ref/ zip_accel: zip_accel {
		compatible = "zmk,input-processor-accel";
		#input-processor-cells = <0>;
		type = <INPUT_EV_REL>;
		x-code = <INPUT_REL_X>;
		y-code = <INPUT_REL_Y>;
	};
};
```

- [ ] **Step 3: Write the processor**

Create `src/pointing/input_processor_accel.c`:

```c
/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_accel

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>

#include "accel_math.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ACCEL_MAX_DEVICES CONFIG_ZMK_INPUT_PROCESSOR_ACCEL_MAX_DEVICES

struct accel_config {
    uint8_t type;
    uint16_t x_code;
    uint16_t y_code;
    const uint16_t *curve;
    struct zmk_accel_axis_config x;
    struct zmk_accel_axis_config y;
};

struct accel_data {
    struct zmk_accel_axis_state x[ACCEL_MAX_DEVICES];
    struct zmk_accel_axis_state y[ACCEL_MAX_DEVICES];
};

static int accel_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                              uint32_t param2, struct zmk_input_processor_state *state) {
    const struct accel_config *cfg = dev->config;
    struct accel_data *data = dev->data;

    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct zmk_accel_axis_config *axis_cfg;
    struct zmk_accel_axis_state *axis_state;

    if (event->code == cfg->x_code) {
        axis_cfg = &cfg->x;
        axis_state = data->x;
    } else if (event->code == cfg->y_code) {
        axis_cfg = &cfg->y;
        axis_state = data->y;
    } else {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* State is indexed by input device. Anything past the configured bound
     * passes through unaccelerated rather than aliasing another device. */
    if (state->input_device_index >= ACCEL_MAX_DEVICES) {
        LOG_WRN("Device index %d exceeds accel state bound %d", state->input_device_index,
                ACCEL_MAX_DEVICES);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    event->value = zmk_accel_apply(cfg->curve, axis_cfg, &axis_state[state->input_device_index],
                                  event->value, k_uptime_get_32());

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api accel_driver_api = {
    .handle_event = accel_handle_event,
};

#define ACCEL_CURVE_DEF(n)                                                                         \
    IF_ENABLED(DT_INST_NODE_HAS_PROP(n, accel_curve),                                              \
               (static const uint16_t accel_curve_##n[] = DT_INST_PROP(n, accel_curve);            \
                BUILD_ASSERT(DT_INST_PROP_LEN(n, accel_curve) == ZMK_ACCEL_CURVE_LEN,              \
                             "accel-curve must have exactly 30 entries");                          \
                BUILD_ASSERT(DT_INST_PROP_BY_IDX(n, accel_curve, 0) == 0,                          \
                             "accel-curve index 0 must be 0");))

#define ACCEL_CURVE_PTR(n)                                                                         \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(n, accel_curve), (accel_curve_##n),                          \
                (zmk_accel_default_curve))

#define ACCEL_INST(n)                                                                              \
    ACCEL_CURVE_DEF(n)                                                                             \
    BUILD_ASSERT(DT_INST_PROP(n, move_inertia_x) < 100, "move-inertia-x must be 0-99");            \
    BUILD_ASSERT(DT_INST_PROP(n, move_inertia_y) < 100, "move-inertia-y must be 0-99");            \
    BUILD_ASSERT(DT_INST_PROP(n, max_accel_x) <= 2255, "max-accel-x saturates int16 above 2255");  \
    BUILD_ASSERT(DT_INST_PROP(n, max_accel_y) <= 2255, "max-accel-y saturates int16 above 2255");  \
    static struct accel_data accel_data_##n = {};                                                  \
    static const struct accel_config accel_config_##n = {                                          \
        .type = DT_INST_PROP(n, type),                                                             \
        .x_code = DT_INST_PROP(n, x_code),                                                         \
        .y_code = DT_INST_PROP(n, y_code),                                                         \
        .curve = ACCEL_CURVE_PTR(n),                                                               \
        .x = {.max_accel = DT_INST_PROP(n, max_accel_x),                                           \
              .exactness = DT_INST_PROP(n, exactness_x),                                           \
              .inertia = DT_INST_PROP(n, move_inertia_x)},                                         \
        .y = {.max_accel = DT_INST_PROP(n, max_accel_y),                                           \
              .exactness = DT_INST_PROP(n, exactness_y),                                           \
              .inertia = DT_INST_PROP(n, move_inertia_y)},                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &accel_data_##n, &accel_config_##n, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &accel_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST)
```

- [ ] **Step 4: Wire the build**

Create `src/pointing/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: MIT

# Input processors compile into ZMK's `app` target, not into a zephyr_library:
# <drivers/input_processor.h> lives in zmk/app/include, which app/CMakeLists.txt
# marks PRIVATE to that target. This is deliberately unlike drivers/input, which
# uses zephyr_library_amend() to extend Zephyr's own `input` library.
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_ACCEL app PRIVATE input_processor_accel.c)
```

Create `src/pointing/Kconfig`:

```
# SPDX-License-Identifier: MIT

if ZMK_POINTING

config ZMK_INPUT_PROCESSOR_ACCEL
    bool "Trackball acceleration input processor"
    default y
    depends on DT_HAS_ZMK_INPUT_PROCESSOR_ACCEL_ENABLED
    help
      Velocity-dependent acceleration with per-axis smoothing for relative
      pointing devices. Restores the gain curve and averaging the ds87 fork
      applied before the ZMK v0.3.0 migration.

config ZMK_INPUT_PROCESSOR_ACCEL_MAX_DEVICES
    int "Input devices tracked per accel processor instance"
    default 4
    depends on ZMK_INPUT_PROCESSOR_ACCEL
    help
      Smoothing state is kept per input device index. Events from a device at
      or above this index pass through unaccelerated.

endif # ZMK_POINTING
```

Modify `CMakeLists.txt` — append after `add_subdirectory(drivers)`:

```cmake
add_subdirectory(src/pointing)
```

Modify `Kconfig` — append after the existing `rsource`:

```
rsource "src/pointing/Kconfig"
```

- [ ] **Step 5: Wire the overlay**

In `config/corne_right.overlay`, add the include after the existing `<input/processors.dtsi>` line:

```c
/* Our own processor, resolved via this module's dts_root. */
#include <input/processors/accel.dtsi>
```

Then replace the `input-processors` line inside `trackball_listener`:

```dts
		input-processors = <&zip_x_scaler 11 10>, <&zip_y_scaler 11 10>, <&zip_accel>;
```

Leave the `scroll` child node exactly as it is. On the `SCROLL` layer the base chain is skipped entirely (`input_listener.c:223` returns early when a layer override matches), so acceleration never touches scrolling.

- [ ] **Step 6: Verify the math tests still pass**

Run: `./tests/accel/run.sh`
Expected: PASS — `all checks passed`. The header is shared, so this catches any edit made while wiring.

- [ ] **Step 7: Verify the devicetree parses**

Run: `git add -A && git status --short`
Expected: the eight files above, staged.

Push the branch and confirm the GitHub Actions build for `corne_right nice_view_adapter nice_view_gem` reaches `Configuring done` without a `devicetree error` or `gen_defines.py failed`. Confirm the build log's `-- Found devicetree overlay:` lines include `config/corne_right.overlay`.

If `ip_zero_param.yaml` fails to resolve, the binding is not finding ZMK's bindings directory — drop the `include:` line and declare `#input-processor-cells` (`type: int`, `required: true`, `const: 0`) and `track-remainders` (`type: boolean`) directly in our binding instead.

- [ ] **Step 8: Commit**

```bash
git commit -m "feat: add zip_accel input processor and wire up the trackball

Restores the ds87 fork's acceleration as a real input processor with per-axis
tuning. Defaults reproduce the fork's feel exactly; move-factor becomes
per-axis by splitting zip_xy_scaler into zip_x_scaler and zip_y_scaler."
```

- [ ] **Step 9: Flash and check the feel**

Flash the right half. Confirm: the ball moves the cursor; slow nudges are controllable; fast sweeps cross the screen; motion does not coast after the ball stops; holding `&mo SCROLL` still scrolls at the previous speed.

Retuning from here is devicetree only — `max-accel-x/y` for overall speed, `exactness-x/y` for fine control, `move-inertia-x/y` for smoothness, and the scaler ratios for raw per-axis sensitivity.

---

## Notes for the executor

- **The v0.3.0 pin is deliberate.** `@main` plus the `nice_nano_v2` board rename is a second migration, not a bump. Do not touch `config/west.yml`.
- **`accel_math.h` is shared by the firmware and the host tests.** Any Zephyr header added to it breaks `tests/accel/run.sh`.
- **Task 1 stands alone.** If CI needs to go green before the rest is ready, Task 1 is a complete fix by itself.
