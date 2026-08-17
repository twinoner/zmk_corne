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

int main(void) {
    test_curve_lookup();
    test_curve_max_accel();
    test_curve_exactness_bypass();
    test_no_inertia_passes_curve_through();
    test_inertia_settles_to_curve_value();
    test_fraction_carry_preserves_displacement();
    test_idle_reset_clears_stale_velocity();
    test_first_event_starts_from_rest();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("all checks passed\n");
    return 0;
}
