/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_click_lock

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "click_lock.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_click_lock_binding_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    /* Does not release a lock that is currently engaged, on purpose: the next
     * trackball press still lets go of it (see zmk_click_lock_set_mode), and
     * releasing from here would mean poking the HID button state behind the
     * input listener's back. */
    bool mode = zmk_click_lock_toggle_mode(&zmk_click_lock);

    LOG_DBG("click lock %s", mode ? "on" : "off");

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_click_lock_binding_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_click_lock_driver_api = {
    .binding_pressed = on_click_lock_binding_pressed,
    .binding_released = on_click_lock_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define CLICK_LOCK_INST(n)                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_click_lock_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CLICK_LOCK_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
