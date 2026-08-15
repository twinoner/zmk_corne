/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pimoroni_pim447

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pim447, CONFIG_INPUT_LOG_LEVEL);

struct pim447_config {
    struct i2c_dt_spec i2c;
    uint32_t poll_interval_ms;
};

struct pim447_data {
    const struct device *dev;
};

static int pim447_init(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus %s is not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    data->dev = dev;

    LOG_INF("PIM447 registered on %s at 0x%02x, poll interval %u ms", cfg->i2c.bus->name,
            cfg->i2c.addr, cfg->poll_interval_ms);

    return 0;
}

#define PIM447_INST(n)                                                                             \
    static struct pim447_data pim447_data_##n = {};                                                \
    static const struct pim447_config pim447_config_##n = {                                        \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .poll_interval_ms = DT_INST_PROP(n, poll_interval),                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, pim447_init, NULL, &pim447_data_##n, &pim447_config_##n, POST_KERNEL, \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PIM447_INST)
