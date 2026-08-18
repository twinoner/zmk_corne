/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_click_lock

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>

#include "click_lock.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Shared with behavior_click_lock.c; see the note in click_lock.h. */
struct zmk_click_lock_state zmk_click_lock;

struct click_lock_config {
    uint8_t type;
    uint16_t code;
};

static int click_lock_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    const struct click_lock_config *cfg = dev->config;

    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != cfg->type || event->code != cfg->code) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    switch (zmk_click_lock_handle(&zmk_click_lock, event->value > 0)) {
    case ZMK_CLICK_LOCK_RELEASE:
        event->value = 0;
        return ZMK_INPUT_PROC_CONTINUE;

    case ZMK_CLICK_LOCK_SUPPRESS:
        /* Dropping the event drops its sync flag with it, so any X/Y reported
         * earlier in the same poll stays in the listener's accumulator until
         * the next synced event. That delays movement by at most one poll
         * interval, and only on polls where the button changed state. */
        return ZMK_INPUT_PROC_STOP;

    case ZMK_CLICK_LOCK_PASS:
    default:
        return ZMK_INPUT_PROC_CONTINUE;
    }
}

static struct zmk_input_processor_driver_api click_lock_driver_api = {
    .handle_event = click_lock_handle_event,
};

#define CLICK_LOCK_INST(n)                                                                         \
    static const struct click_lock_config click_lock_config_##n = {                                \
        .type = DT_INST_PROP(n, type),                                                             \
        .code = DT_INST_PROP(n, code),                                                             \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &click_lock_config_##n, POST_KERNEL,                \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &click_lock_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CLICK_LOCK_INST)
