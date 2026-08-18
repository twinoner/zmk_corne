/*
 * Click-lock state machine for the trackball button.
 *
 * In momentary mode the physical button edges pass through untouched. In lock
 * mode the first click latches the button down and the second releases it,
 * which is the drag-lock behaviour the ds87 fork had before the ZMK v0.3.0
 * migration.
 *
 * The state machine is pure: no Zephyr dependencies, so tests/click_lock can
 * compile this header natively. Keep it that way.
 *
 * `mode` is written from the keymap thread by the behavior and read from the
 * input thread by the processor. A bool needs no locking here: a racing read
 * sees either the old or the new mode, and either one is a valid click.
 *
 * One gap worth knowing about: the driver's emergency button release, which
 * fires when the I2C bus dies mid-press, is suppressed while a lock is
 * engaged, because to this state machine it looks like the ordinary physical
 * release of a latched click. A trackball that dies while locked therefore
 * leaves the host holding the button. Pressing and releasing &mkp MB1 clears
 * it, since that goes to the HID through its own listener.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

enum zmk_click_lock_action {
    /* Forward the event unchanged. */
    ZMK_CLICK_LOCK_PASS,
    /* Drop the event; the host's view of the button is already correct. */
    ZMK_CLICK_LOCK_SUPPRESS,
    /* Forward the event, but as a release: this press ends a lock. */
    ZMK_CLICK_LOCK_RELEASE,
};

struct zmk_click_lock_state {
    /* Lock mode on/off, flipped by the &click_lock keymap behavior. */
    bool mode;
    /* The host is holding the button because a click latched it. */
    bool latched;
    /* The press that ended a lock is still physically down, so its release
     * has to be swallowed rather than sent as a second release. */
    bool unlatching;
};

static inline void zmk_click_lock_set_mode(struct zmk_click_lock_state *state, bool mode) {
    /* Deliberately does not touch `latched`: a lock engaged before the mode
     * changed still has to be releasable, and zmk_click_lock_handle() keys the
     * release off the latch rather than off the mode so that it is. */
    state->mode = mode;
}

static inline bool zmk_click_lock_toggle_mode(struct zmk_click_lock_state *state) {
    zmk_click_lock_set_mode(state, !state->mode);
    return state->mode;
}

/*
 * Feed one physical button edge in, get back what the host should see. Edges
 * must alternate, which the PIM447 driver guarantees by only reporting on
 * change.
 */
static inline enum zmk_click_lock_action
zmk_click_lock_handle(struct zmk_click_lock_state *state, bool pressed) {
    if (pressed) {
        if (state->latched) {
            state->latched = false;
            state->unlatching = true;
            return ZMK_CLICK_LOCK_RELEASE;
        }

        /* Latching is decided here, at press time, so flipping the mode while
         * a button is already down cannot retroactively latch it. */
        state->latched = state->mode;
        state->unlatching = false;
        return ZMK_CLICK_LOCK_PASS;
    }

    if (state->unlatching) {
        state->unlatching = false;
        return ZMK_CLICK_LOCK_SUPPRESS;
    }

    if (state->latched) {
        return ZMK_CLICK_LOCK_SUPPRESS;
    }

    return ZMK_CLICK_LOCK_PASS;
}

/*
 * The one shared instance, defined in input_processor_click_lock.c. The
 * processor and the &click_lock behavior are separate translation units (a
 * file can only have one DT_DRV_COMPAT) but must agree on one mode flag, and
 * the processor is deliberately referenced from more than one chain in
 * corne_right.overlay, so per-node state would be wrong here anyway.
 *
 * Not used by the host tests, which drive their own local state.
 */
extern struct zmk_click_lock_state zmk_click_lock;
