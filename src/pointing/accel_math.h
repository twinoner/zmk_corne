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
