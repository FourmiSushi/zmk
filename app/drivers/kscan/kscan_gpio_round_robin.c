/*
 * Copyright (c) 2020-2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "debounce.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_kscan_gpio_round_robin

#define INST_LEN(n) DT_INST_PROP_LEN(n, gpios)
#define INST_ROUND_ROBIN_LEN(n) (INST_LEN(n) * INST_LEN(n))

#if CONFIG_ZMK_KSCAN_DEBOUNCE_PRESS_MS >= 0
#define INST_DEBOUNCE_PRESS_MS(n) CONFIG_ZMK_KSCAN_DEBOUNCE_PRESS_MS
#else
#define INST_DEBOUNCE_PRESS_MS(n)                                                                  \
    DT_INST_PROP_OR(n, debounce_period, DT_INST_PROP(n, debounce_press_ms))
#endif

#if CONFIG_ZMK_KSCAN_DEBOUNCE_RELEASE_MS >= 0
#define INST_DEBOUNCE_RELEASE_MS(n) CONFIG_ZMK_KSCAN_DEBOUNCE_RELEASE_MS
#else
#define INST_DEBOUNCE_RELEASE_MS(n)                                                                \
    DT_INST_PROP_OR(n, debounce_period, DT_INST_PROP(n, debounce_release_ms))
#endif

#define KSCAN_GPIO_CFG_INIT(idx, inst_idx)                                                         \
    GPIO_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst_idx), gpios, idx),

#define INST_INTR_DEFINED(n) DT_INST_NODE_HAS_PROP(n, interrupt_gpios)

#define WITH_INTR(n) COND_CODE_1(INST_INTR_DEFINED(n), (+1), (+0))
#define WITHOUT_INTR(n) COND_CODE_0(INST_INTR_DEFINED(n), (+1), (+0))

#define USES_POLLING DT_INST_FOREACH_STATUS_OKAY(WITHOUT_INTR) > 0
#define USES_INTERRUPT DT_INST_FOREACH_STATUS_OKAY(WITH_INTR) > 0

#if USES_POLLING && USES_INTERRUPT
#define USES_POLL_AND_INTR 1
#else
#define USES_POLL_AND_INTR 0
#endif

#define COND_ANY_POLLING(code) COND_CODE_1(USES_POLLING, code, ())
#define COND_POLL_AND_INTR(code) COND_CODE_1(USES_POLL_AND_INTR, code, ())
#define COND_THIS_INTERRUPT(n, code) COND_CODE_1(INST_INTR_DEFINED(n), code, ())

#define KSCAN_INTR_CFG_INIT(inst_idx) GPIO_DT_SPEC_GET(DT_DRV_INST(inst_idx), interrupt_gpios)

#define RETURN_ERR_IF_ERR(before_return)                                                           \
    if (err) {                                                                                     \
        before_return;                                                                             \
        return err;                                                                                \
    }

struct kscan_round_robin_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    int64_t scan_time; /* Timestamp of the current or scheduled scan. */
#if USES_INTERRUPT
    struct gpio_callback irq_callback;
#endif
    /**
     * Current state of the matrix as a flattened 2D array of length
     * (config->cells.length ^2)
     */
    struct debounce_state *round_robin_state;
};

struct kscan_gpio_list {
    const struct gpio_dt_spec *gpios;
    size_t len;
};

/** Define a kscan_gpio_list from a compile-time GPIO array. */
#define KSCAN_GPIO_LIST(gpio_array)                                                                \
    ((struct kscan_gpio_list){.gpios = gpio_array, .len = ARRAY_SIZE(gpio_array)})

struct kscan_round_robin_config {
    struct kscan_gpio_list cells;
    struct debounce_config debounce_config;
    int32_t debounce_scan_period_ms;
#if USES_POLLING
    int32_t poll_period_ms;
#endif
#if USES_POLL_AND_INTR
    bool use_interrupt;
#endif
#if USES_INTERRUPT
    const struct gpio_dt_spec interrupt;
#endif
};

/**
 * Get the index into a matrix state array from a row and column.
 * There are effectively (n) cols and (n-1) rows, but we use the full col x row space
 * as a safety measure against someone accidentally defining a transform RC at (p,p)
 */
static int state_index(const struct kscan_round_robin_config *config, const int row,
                       const int col) {
    __ASSERT(row < config->cells.len, "Invalid row %i", row);
    __ASSERT(col < config->cells.len, "Invalid column %i", col);
    __ASSERT(col != row, "Invalid column row pair %i, %i", col, row);

    return (col * config->cells.len) + row;
}

static int kscan_round_robin_set_as_input(const struct gpio_dt_spec *gpio) {
    if (!device_is_ready(gpio->port)) {
        LOG_ERR("GPIO is not ready: %s", gpio->port->name);
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(gpio, GPIO_INPUT);
    RETURN_ERR_IF_ERR(
        LOG_ERR("Unable to configure pin %u on %s for input", gpio->pin, gpio->port->name))
    return 0;
}

static int kscan_round_robin_set_as_output(const struct gpio_dt_spec *gpio) {
    if (!device_is_ready(gpio->port)) {
        LOG_ERR("GPIO is not ready: %s", gpio->port->name);
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(gpio, GPIO_OUTPUT);
    RETURN_ERR_IF_ERR(
        LOG_ERR("Unable to configure pin %u on %s for output", gpio->pin, gpio->port->name);)

    err = gpio_pin_set_dt(gpio, 1);
    RETURN_ERR_IF_ERR(LOG_ERR("Failed to set output pin %u active: %i", gpio->pin, err);)

    return 0;
}

static int kscan_round_robin_set_all_as_input(const struct device *dev) {
    const struct kscan_round_robin_config *config = dev->config;
    int err = 0;
    for (int i = 0; i < config->cells.len; i++) {
        err = kscan_round_robin_set_as_input(&config->cells.gpios[i]);
        RETURN_ERR_IF_ERR()
    }

    return 0;
}

static int kscan_round_robin_set_all_outputs(const struct device *dev, const int value) {
    const struct kscan_round_robin_config *config = dev->config;

    for (int i = 0; i < config->cells.len; i++) {
        const struct gpio_dt_spec *gpio = &config->cells.gpios[i];
        int err = gpio_pin_configure_dt(gpio, GPIO_OUTPUT);
        RETURN_ERR_IF_ERR(
            LOG_ERR("Unable to configure pin %u on %s for input", gpio->pin, gpio->port->name);)

        err = gpio_pin_set_dt(gpio, value);
        RETURN_ERR_IF_ERR(LOG_ERR("Failed to set output %i to %i: %i", i, value, err);)
    }

    return 0;
}

#if USES_INTERRUPT
static int kscan_round_robin_interrupt_configure(const struct device *dev,
                                                 const gpio_flags_t flags) {
    const struct kscan_round_robin_config *config = dev->config;
    const struct gpio_dt_spec *gpio = &config->interrupt;

    int err = gpio_pin_interrupt_configure_dt(gpio, flags);
    RETURN_ERR_IF_ERR(
        LOG_ERR("Unable to configure interrupt for pin %u on %s", gpio->pin, gpio->port->name);)

    return 0;
}

static int kscan_round_robin_interrupt_enable(const struct device *dev) {
    int err = kscan_round_robin_interrupt_configure(dev, GPIO_INT_LEVEL_ACTIVE);
    RETURN_ERR_IF_ERR()

    // While interrupts are enabled, set all outputs active so an pressed key will trigger
    return kscan_round_robin_set_all_outputs(dev, 1);
}

static void kscan_round_robin_irq_callback(const struct device *port, struct gpio_callback *cb,
                                           const gpio_port_pins_t _pin) {
    struct kscan_round_robin_data *data =
        CONTAINER_OF(cb, struct kscan_round_robin_data, irq_callback);

    // Disable our interrupt to avoid re-entry while we scan.
    kscan_round_robin_interrupt_configure(data->dev, GPIO_INT_DISABLE);
    data->scan_time = k_uptime_get();
    k_work_reschedule(&data->work, K_NO_WAIT);
}
#endif

static void kscan_round_robin_read_continue(const struct device *dev) {
    const struct kscan_round_robin_config *config = dev->config;
    struct kscan_round_robin_data *data = dev->data;

    data->scan_time += config->debounce_scan_period_ms;

    k_work_reschedule(&data->work, K_TIMEOUT_ABS_MS(data->scan_time));
}

static void kscan_round_robin_read_end(const struct device *dev) {
#if USES_POLLING
    struct kscan_round_robin_data *data = dev->data;
    const struct kscan_round_robin_config *config = dev->config;
#endif

#if USES_POLL_AND_INTR
    if (config->use_interrupt) {
#endif
#if USES_INTERRUPT
        // Return to waiting for an interrupt.
        kscan_round_robin_interrupt_enable(dev);
#endif
#if USES_POLL_AND_INTR
    } else {
#endif
#if USES_POLLING
        data->scan_time += config->poll_period_ms;

        // Return to polling slowly.
        k_work_reschedule(&data->work, K_TIMEOUT_ABS_MS(data->scan_time));
#endif
#if USES_POLL_AND_INTR
    }
#endif
}

static int kscan_round_robin_read(const struct device *dev) {
    struct kscan_round_robin_data *data = dev->data;
    const struct kscan_round_robin_config *config = dev->config;
    bool continue_scan = false;

    // NOTE: RR vs MATRIX: set all pins as input, in case there was a failure on a
    // previous scan, and one of the pins is still set as output
    int err = kscan_round_robin_set_all_as_input(dev);
    RETURN_ERR_IF_ERR()

    // Scan the matrix.
    for (int row = 0; row < config->cells.len; row++) {
        const struct gpio_dt_spec *out_gpio = &config->cells.gpios[row];
        err = kscan_round_robin_set_as_output(out_gpio);
        RETURN_ERR_IF_ERR()

#if CONFIG_ZMK_KSCAN_ROUND_ROBIN_WAIT_BEFORE_INPUTS > 0
        k_busy_wait(CONFIG_ZMK_KSCAN_ROUND_ROBIN_WAIT_BEFORE_INPUTS);
#endif

        for (int col = 0; col < config->cells.len; col++) {
            if (col == row) {
                continue; // pin can't drive itself
            }
            const struct gpio_dt_spec *in_gpio = &config->cells.gpios[col];
            const int index = state_index(config, row, col);

            struct debounce_state *state = &data->round_robin_state[index];
            debounce_update(state, gpio_pin_get_dt(in_gpio), config->debounce_scan_period_ms,
                            &config->debounce_config);

            // NOTE: RR vs MATRIX: because we don't need an input/output => row/column
            // setup, we can update in the same loop.
            if (debounce_get_changed(state)) {
                const bool pressed = debounce_is_pressed(state);

                LOG_DBG("Sending event at %i,%i state %s", row, col, pressed ? "on" : "off");
                data->callback(dev, row, col, pressed);
            }
            continue_scan = continue_scan || debounce_is_active(state);
        }

        err = kscan_round_robin_set_as_input(out_gpio);
        RETURN_ERR_IF_ERR()

#if CONFIG_ZMK_KSCAN_ROUND_ROBIN_WAIT_BETWEEN_OUTPUTS > 0
        k_busy_wait(CONFIG_ZMK_KSCAN_ROUND_ROBIN_WAIT_BETWEEN_OUTPUTS);
#endif
    }

    if (continue_scan) {
        // At least one key is pressed or the debouncer has not yet decided if
        // it is pressed. Poll quickly until everything is released.
        kscan_round_robin_read_continue(dev);
    } else {
        // All keys are released. Return to normal.
        kscan_round_robin_read_end(dev);
    }

    return 0;
}

static void kscan_round_robin_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_round_robin_data *data = CONTAINER_OF(dwork, struct kscan_round_robin_data, work);
    kscan_round_robin_read(data->dev);
}

static int kscan_round_robin_configure(const struct device *dev, const kscan_callback_t callback) {
    if (!callback) {
        return -EINVAL;
    }

    struct kscan_round_robin_data *data = dev->data;
    data->callback = callback;
    return 0;
}

static int kscan_round_robin_enable(const struct device *dev) {
    struct kscan_round_robin_data *data = dev->data;
    data->scan_time = k_uptime_get();

    // Read will automatically start interrupts/polling once done.
    return kscan_round_robin_read(dev);
}

static int kscan_round_robin_disable(const struct device *dev) {
    struct kscan_round_robin_data *data = dev->data;
    k_work_cancel_delayable(&data->work);

#if USES_POLL_AND_INTR
    const struct kscan_round_robin_config *config = dev->config;
    if (config->use_interrupt) {
#endif
#if USES_INTERRUPT
        return kscan_round_robin_interrupt_configure(dev, GPIO_INT_DISABLE);
#endif
#if USES_POLL_AND_INTR
    }
#endif

#if USES_POLLING
    return 0;
#endif
}

static int kscan_round_robin_init_inputs(const struct device *dev) {
    const struct kscan_round_robin_config *config = dev->config;

    for (int i = 0; i < config->cells.len; i++) {
        int err = kscan_round_robin_set_as_input(&config->cells.gpios[i]);
        RETURN_ERR_IF_ERR()
    }

    return 0;
}

#if USES_INTERRUPT
static int kscan_round_robin_init_interrupt(const struct device *dev) {
    struct kscan_round_robin_data *data = dev->data;

    const struct kscan_round_robin_config *config = dev->config;
    const struct gpio_dt_spec *gpio = &config->interrupt;
    int err = kscan_round_robin_set_as_input(gpio);
    RETURN_ERR_IF_ERR()

    gpio_init_callback(&data->irq_callback, kscan_round_robin_irq_callback, BIT(gpio->pin));
    err = gpio_add_callback(gpio->port, &data->irq_callback);
    RETURN_ERR_IF_ERR(LOG_ERR("Error adding the callback to the input device: %i", err))

    return 0;
}
#endif

static int kscan_round_robin_init(const struct device *dev) {
    struct kscan_round_robin_data *data = dev->data;

    data->dev = dev;

    kscan_round_robin_init_inputs(dev);
    kscan_round_robin_set_all_outputs(dev, 0);

#if USES_POLL_AND_INTR
    const struct kscan_round_robin_config *config = dev->config;
    if (config->use_interrupt) {
#endif
#if USES_INTERRUPT
        kscan_round_robin_init_interrupt(dev);
#endif
#if USES_POLL_AND_INTR
    }
#endif

    k_work_init_delayable(&data->work, kscan_round_robin_work_handler);

    return 0;
}

static const struct kscan_driver_api kscan_round_robin_api = {
    .config = kscan_round_robin_configure,
    .enable_callback = kscan_round_robin_enable,
    .disable_callback = kscan_round_robin_disable,
};

#define KSCAN_ROUND_ROBIN_INIT(n)                                                                  \
    BUILD_ASSERT(INST_DEBOUNCE_PRESS_MS(n) <= DEBOUNCE_COUNTER_MAX,                                \
                 "ZMK_KSCAN_DEBOUNCE_PRESS_MS or debounce-press-ms is too large");                 \
    BUILD_ASSERT(INST_DEBOUNCE_RELEASE_MS(n) <= DEBOUNCE_COUNTER_MAX,                              \
                 "ZMK_KSCAN_DEBOUNCE_RELEASE_MS or debounce-release-ms is too large");             \
                                                                                                   \
    static struct debounce_state kscan_round_robin_state_##n[INST_ROUND_ROBIN_LEN(n)];             \
    static const struct gpio_dt_spec kscan_round_robin_cells_##n[] = {                             \
        UTIL_LISTIFY(INST_LEN(n), KSCAN_GPIO_CFG_INIT, n)};                                        \
    static struct kscan_round_robin_data kscan_round_robin_data_##n = {                            \
        .round_robin_state = kscan_round_robin_state_##n,                                          \
    };                                                                                             \
                                                                                                   \
    static struct kscan_round_robin_config kscan_round_robin_config_##n = {                        \
        .cells = KSCAN_GPIO_LIST(kscan_round_robin_cells_##n),                                     \
        .debounce_config =                                                                         \
            {                                                                                      \
                .debounce_press_ms = INST_DEBOUNCE_PRESS_MS(n),                                    \
                .debounce_release_ms = INST_DEBOUNCE_RELEASE_MS(n),                                \
            },                                                                                     \
        .debounce_scan_period_ms = DT_INST_PROP(n, debounce_scan_period_ms),                       \
        COND_ANY_POLLING((.poll_period_ms = DT_INST_PROP(n, poll_period_ms), ))                    \
            COND_POLL_AND_INTR((.use_interrupt = INST_INTR_DEFINED(n), ))                          \
                COND_THIS_INTERRUPT(n, (.interrupt = KSCAN_INTR_CFG_INIT(n), ))};                  \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, &kscan_round_robin_init, NULL, &kscan_round_robin_data_##n,           \
                          &kscan_round_robin_config_##n, APPLICATION,                              \
                          CONFIG_APPLICATION_INIT_PRIORITY, &kscan_round_robin_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_ROUND_ROBIN_INIT);