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
