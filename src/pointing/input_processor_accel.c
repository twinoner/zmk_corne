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
    BUILD_ASSERT(DT_INST_PROP(n, move_inertia_x) >= 0 && DT_INST_PROP(n, move_inertia_x) < 100,    \
                 "move-inertia-x must be 0-99");                                                    \
    BUILD_ASSERT(DT_INST_PROP(n, move_inertia_y) >= 0 && DT_INST_PROP(n, move_inertia_y) < 100,    \
                 "move-inertia-y must be 0-99");                                                    \
    BUILD_ASSERT(DT_INST_PROP(n, max_accel_x) >= 0 && DT_INST_PROP(n, max_accel_x) <= 2255,        \
                 "max-accel-x must be 0-2255; it saturates int16 above 2255");                     \
    BUILD_ASSERT(DT_INST_PROP(n, max_accel_y) >= 0 && DT_INST_PROP(n, max_accel_y) <= 2255,        \
                 "max-accel-y must be 0-2255; it saturates int16 above 2255");                     \
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
