/*
 * Pure-integer acceleration math for the zip_accel input processor.
 *
 * No Zephyr dependencies: this header is compiled both into the firmware and
 * natively by tests/accel. Keep it that way.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <limits.h>
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
