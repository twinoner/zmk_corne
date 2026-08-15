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

/* Register map, following Pimoroni's PIM447 firmware register layout.
 * Delta counters are cleared by the device on read. */
#define PIM447_REG_LEFT 0x04

/* Buffer indices for the burst read starting at PIM447_REG_LEFT. LEFT,
 * RIGHT, UP, DOWN and SWITCH are consecutive registers, so the index into
 * the read buffer doubles as the offset from PIM447_REG_LEFT. */
enum {
    PIM447_IDX_LEFT = 0,
    PIM447_IDX_RIGHT,
    PIM447_IDX_UP,
    PIM447_IDX_DOWN,
    PIM447_IDX_SWITCH,
    PIM447_IDX_COUNT,
};

/* Bit 7 of the switch register is the current button state; the low bits
 * are a press counter we do not use. */
#define PIM447_SWITCH_PRESSED BIT(7)

/* Consecutive read failures before the poll backs off from 50 Hz to
 * roughly 1 Hz. Kept low so a merely flaky bus still feels responsive. */
#define PIM447_ERR_BACKOFF_THRESHOLD 5

struct pim447_config {
    struct i2c_dt_spec i2c;
    uint32_t poll_interval_ms;
};

struct pim447_data {
    const struct device *dev;
    struct k_work_delayable work;
    bool btn_pressed;
    uint8_t err_count;
};

static void pim447_poll(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct pim447_data *data = CONTAINER_OF(dwork, struct pim447_data, work);
    const struct device *dev = data->dev;
    const struct pim447_config *cfg = dev->config;
    uint8_t buf[PIM447_IDX_COUNT];

    int ret = i2c_burst_read_dt(&cfg->i2c, PIM447_REG_LEFT, buf, sizeof(buf));
    if (ret < 0) {
        /* Cap err_count at the backoff threshold + 1 instead of letting it
         * free-run: a permanently dead trackball would otherwise wrap the
         * uint8_t back through zero and bounce out of backoff. */
        if (data->err_count <= PIM447_ERR_BACKOFF_THRESHOLD) {
            data->err_count++;
        }

        if (data->err_count == 1) {
            LOG_ERR("Failed to read PIM447 registers: %d", ret);
        }

        /* Don't leave the host holding a phantom click if the bus dies
         * mid-press; nothing else on this path can release it. Retried on
         * every failed poll rather than only the first, because a full input
         * queue drops the report silently under K_NO_WAIT — clearing the flag
         * regardless would strand the host holding the button. */
        if (data->btn_pressed &&
            input_report_key(dev, INPUT_BTN_0, false, true, K_NO_WAIT) == 0) {
            data->btn_pressed = false;
        }

        goto reschedule;
    }

    data->err_count = 0;

    int16_t dx = (int16_t)buf[PIM447_IDX_RIGHT] - (int16_t)buf[PIM447_IDX_LEFT];
    int16_t dy = (int16_t)buf[PIM447_IDX_DOWN] - (int16_t)buf[PIM447_IDX_UP];
    bool pressed = (buf[PIM447_IDX_SWITCH] & PIM447_SWITCH_PRESSED) != 0;
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
    if (data->err_count > PIM447_ERR_BACKOFF_THRESHOLD) {
        k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms * 50));
    } else {
        k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
    }
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
