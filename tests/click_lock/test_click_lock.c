/*
 * Host tests for the click-lock state machine. No Zephyr: compiled with plain
 * cc, same as tests/accel.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "click_lock.h"

static int failures;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define PRESS(s) zmk_click_lock_handle(s, true)
#define RELEASE(s) zmk_click_lock_handle(s, false)

/* Momentary mode is the boot default and must behave exactly like no
 * processor at all: every edge reaches the host unchanged. */
static void test_momentary_passes_both_edges(void) {
    struct zmk_click_lock_state s = {};

    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_PASS);

    /* Repeated clicks keep passing; no state accumulates. */
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_PASS);
}

/* The feature itself: click once to lock the button down, click again to
 * release it. The host must see exactly one press and one release across the
 * two physical clicks. */
static void test_lock_mode_latches_then_releases(void) {
    struct zmk_click_lock_state s = {};
    zmk_click_lock_set_mode(&s, true);

    /* First click: the press reaches the host, the release does not. */
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);

    /* Second click: the press is rewritten into the release, and that click's
     * own release is swallowed so the host sees only one. */
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_RELEASE);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);

    /* And the cycle repeats cleanly. */
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_RELEASE);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);
}

/* Turning the mode off must never strand the host holding the button: the
 * release path keys off the latch, not the mode, so the next ball press still
 * lets go. */
static void test_mode_off_while_latched_still_releases(void) {
    struct zmk_click_lock_state s = {};
    zmk_click_lock_set_mode(&s, true);

    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);

    zmk_click_lock_set_mode(&s, false);

    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_RELEASE);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);

    /* Back to plain momentary from here on. */
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_PASS);
}

/* Turning the mode on mid-click must not retroactively latch a click that
 * started as momentary, or the button sticks down with no press to blame. */
static void test_mode_on_mid_click_does_not_latch_it(void) {
    struct zmk_click_lock_state s = {};

    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);

    zmk_click_lock_set_mode(&s, true);

    /* The in-flight click completes as a normal click. */
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_PASS);

    /* The next one latches. */
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_SUPPRESS);
}

/* Toggling is what the keymap key does, so it has to survive being mashed. */
static void test_toggle_flips_mode(void) {
    struct zmk_click_lock_state s = {};

    CHECK(zmk_click_lock_toggle_mode(&s) == true);
    CHECK(s.mode == true);
    CHECK(zmk_click_lock_toggle_mode(&s) == false);
    CHECK(s.mode == false);

    /* Toggling on and back off between clicks leaves momentary behavior. */
    CHECK(zmk_click_lock_toggle_mode(&s) == true);
    CHECK(zmk_click_lock_toggle_mode(&s) == false);
    CHECK(PRESS(&s) == ZMK_CLICK_LOCK_PASS);
    CHECK(RELEASE(&s) == ZMK_CLICK_LOCK_PASS);
}

int main(void) {
    test_momentary_passes_both_edges();
    test_lock_mode_latches_then_releases();
    test_mode_off_while_latched_still_releases();
    test_mode_on_mid_click_does_not_latch_it();
    test_toggle_flips_mode();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("all click_lock tests passed\n");
    return 0;
}
