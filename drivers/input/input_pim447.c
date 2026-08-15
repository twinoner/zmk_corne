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

/* Register map. Delta counters are cleared by the device on read. */
#define PIM447_REG_LEFT   0x04
#define PIM447_REG_RIGHT  0x05
#define PIM447_REG_UP     0x06
#define PIM447_REG_DOWN   0x07
#define PIM447_REG_SWITCH 0x08

#define PIM447_REG_COUNT (PIM447_REG_SWITCH - PIM447_REG_LEFT + 1)

/* Bit 7 of the switch register is the current button state; the low bits
 * are a press counter we do not use. */
#define PIM447_SWITCH_PRESSED BIT(7)

struct pim447_config {
    struct i2c_dt_spec i2c;
    uint32_t poll_interval_ms;
};

struct pim447_data {
    const struct device *dev;
    struct k_work_delayable work;
    bool btn_pressed;
};

static void pim447_poll(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dwork, struct pim447_data, work);
    const struct device *dev = data->dev;
    const struct pim447_config *cfg = dev->config;
    uint8_t buf[PIM447_REG_COUNT];

    int ret = i2c_burst_read_dt(&cfg->i2c, PIM447_REG_LEFT, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Failed to read PIM447 registers: %d", ret);
        goto reschedule;
    }

    int16_t dx = (int16_t)buf[1] - (int16_t)buf[0]; /* RIGHT - LEFT */
    int16_t dy = (int16_t)buf[3] - (int16_t)buf[2]; /* DOWN  - UP   */
    bool pressed = (buf[4] & PIM447_SWITCH_PRESSED) != 0;
    bool btn_changed = pressed != data->btn_pressed;

    /* Stay silent when nothing happened so the listener is not woken on
     * every poll. */
    if (dx != 0 || dy != 0) {
        input_report_rel(dev, INPUT_REL_X, dx, false, K_NO_WAIT);
        input_report_rel(dev, INPUT_REL_Y, dy, !btn_changed, K_NO_WAIT);
    }

    if (btn_changed && input_report_key(dev, INPUT_BTN_0, pressed, true, K_NO_WAIT) == 0) {
        data->btn_pressed = pressed;
    }

reschedule:
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}

static int pim447_init(const struct device *dev) {
    const struct pim447_config *cfg = dev->config;
    struct pim447_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus %s is not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    data->dev = dev;

    k_work_init_delayable(&data->work, pim447_poll);
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));

    LOG_INF("PIM447 polling on %s at 0x%02x every %u ms", cfg->i2c.bus->name, cfg->i2c.addr,
            cfg->poll_interval_ms);

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
