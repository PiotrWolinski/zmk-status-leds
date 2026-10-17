/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_status_leds

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk_status_leds/status_leds.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_status_leds_config {
    bool indicate_battery;
    bool indicate_bt;
};

static int behavior_status_leds_init(const struct device *dev) {
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_status_leds_config *cfg = dev->config;

    if (cfg->indicate_battery) {
        zmk_status_leds_show_battery();
    }

    if (cfg->indicate_bt) {
        zmk_status_leds_show_bt();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_status_leds_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define STATUS_LEDS_INST(n)                                                                        \
    static const struct behavior_status_leds_config behavior_status_leds_config_##n = {            \
        .indicate_battery = DT_INST_PROP(n, indicate_battery),                                     \
        .indicate_bt = DT_INST_PROP(n, indicate_bt),                                               \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_status_leds_init, NULL, NULL,                              \
                            &behavior_status_leds_config_##n, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_status_leds_driver_api);

DT_INST_FOREACH_STATUS_OKAY(STATUS_LEDS_INST)
