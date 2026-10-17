/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_status_leds

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
#include <drivers/ext_power.h>
#endif

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#include <zmk_status_leds/status_leds.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* Resolve the physical LED strip node:
 * 1. Explicit `led-strip = <&led_strip>;` property on the zmk,status-leds node
 * 2. Fallback to `chosen { zmk,underglow = &led_strip; };`
 * 3. Fallback to node label `led_strip`
 */
#if DT_INST_NODE_HAS_PROP(0, led_strip)
#define STRIP_NODE DT_INST_PHANDLE(0, led_strip)
#elif DT_HAS_CHOSEN(zmk_underglow)
#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#else
#define STRIP_NODE DT_NODELABEL(led_strip)
#endif

#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *const led_strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

static const int32_t battery_leds[] = DT_INST_PROP(0, battery_leds);
static const size_t battery_led_count = DT_INST_PROP_LEN(0, battery_leds);

static const int32_t bt_leds[] = DT_INST_PROP(0, bt_leds);
static const size_t bt_led_count = DT_INST_PROP_LEN(0, bt_leds);

#define BRT CONFIG_ZMK_STATUS_LEDS_BRIGHTNESS
#define DIM_BRT MAX(1, (CONFIG_ZMK_STATUS_LEDS_BRIGHTNESS / 4))

static const struct led_rgb COLOR_OFF     = { .r = 0,       .g = 0,              .b = 0 };
static const struct led_rgb COLOR_GREEN   = { .r = 0,       .g = BRT,            .b = 0 };
static const struct led_rgb COLOR_ORANGE  = { .r = BRT,     .g = MAX(1, BRT / 3),.b = 0 };
static const struct led_rgb COLOR_RED     = { .r = BRT,     .g = 0,              .b = 0 };
static const struct led_rgb COLOR_BLUE    = { .r = 0,       .g = 0,              .b = BRT };
static const struct led_rgb COLOR_YELLOW  = { .r = BRT,     .g = BRT,            .b = 0 };
static const struct led_rgb COLOR_SLOT_BG = { .r = DIM_BRT, .g = DIM_BRT,        .b = DIM_BRT };

enum status_mode {
    STATUS_MODE_IDLE = 0,
    STATUS_MODE_BATTERY,
    STATUS_MODE_BT,
    STATUS_MODE_DISCOVER,
};

static enum status_mode current_mode = STATUS_MODE_IDLE;
static bool blink_phase_on = false;
static size_t discover_idx = 0;
static int64_t display_end_time_ms = 0;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
static bool underglow_was_on = false;
#endif

static struct k_work_delayable status_tick_work;

static void set_pixel_safe(int32_t idx, struct led_rgb color) {
    if (idx >= 0 && idx < STRIP_NUM_PIXELS) {
        pixels[idx] = color;
    }
}

static void flush_strip(void) {
    if (!device_is_ready(led_strip)) {
        LOG_ERR("LED strip device is not ready");
        return;
    }
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update LED strip (%d)", err);
    }
}

static void clear_strip(void) {
    memset(pixels, 0, sizeof(pixels));
    flush_strip();
}

static void acquire_strip(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    if (current_mode == STATUS_MODE_IDLE) {
        if (zmk_rgb_underglow_get_state(&underglow_was_on) == 0 && underglow_was_on) {
            zmk_rgb_underglow_off();
        }
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL && device_is_ready(ext_power)) {
        ext_power_enable(ext_power);
    }
#endif
}

static void release_strip(void) {
    clear_strip();

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    if (underglow_was_on) {
        underglow_was_on = false;
        zmk_rgb_underglow_on();
        return;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL && device_is_ready(ext_power)) {
        ext_power_disable(ext_power);
    }
#endif
}

static void render_battery_frame(void) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    uint8_t soc = zmk_battery_state_of_charge();

    /* Calculate number of filled LEDs proportionally (rounded to nearest slot) */
    size_t filled_count = (soc * battery_led_count + 50) / 100;
    if (filled_count > battery_led_count) {
        filled_count = battery_led_count;
    }

    struct led_rgb filled_color = (soc < 50) ? COLOR_ORANGE : COLOR_GREEN;

    memset(pixels, 0, sizeof(pixels));
    for (size_t i = 0; i < battery_led_count; i++) {
        set_pixel_safe(battery_leds[i], (i < filled_count) ? filled_color : COLOR_RED);
    }
    flush_strip();
#endif
}

static void status_tick_handler(struct k_work *work) {
    int64_t now = k_uptime_get();

    if (current_mode == STATUS_MODE_DISCOVER) {
        if (discover_idx >= STRIP_NUM_PIXELS) {
            current_mode = STATUS_MODE_IDLE;
            release_strip();
            return;
        }

        memset(pixels, 0, sizeof(pixels));
        /* Color-code milestones to make counting on camera easier:
         * - Every 10th LED (0, 10, 20...): Blue
         * - Every 5th LED (5, 15...):      Green
         * - All other LEDs:                Red
         */
        struct led_rgb marker_color = COLOR_RED;
        if (discover_idx % 10 == 0) {
            marker_color = COLOR_BLUE;
        } else if (discover_idx % 5 == 0) {
            marker_color = COLOR_GREEN;
        }

        pixels[discover_idx] = marker_color;
        flush_strip();

        discover_idx++;
        k_work_reschedule(&status_tick_work, K_MSEC(CONFIG_ZMK_STATUS_LEDS_DISCOVER_STEP_MS));
        return;
    }

    if (now >= display_end_time_ms || current_mode == STATUS_MODE_IDLE) {
        current_mode = STATUS_MODE_IDLE;
        release_strip();
        return;
    }

    if (current_mode == STATUS_MODE_BATTERY) {
        render_battery_frame();
        int64_t remaining_ms = display_end_time_ms - now;
        k_work_reschedule(&status_tick_work, K_MSEC(MAX(1, remaining_ms)));
        return;
    }

    if (current_mode == STATUS_MODE_BT) {
        memset(pixels, 0, sizeof(pixels));

        /* 1. Highlight all configured BT slot keys with a dim background color */
        for (size_t i = 0; i < bt_led_count; i++) {
            set_pixel_safe(bt_leds[i], COLOR_SLOT_BG);
        }

#if IS_ENABLED(CONFIG_ZMK_BLE) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
        uint8_t active_idx = zmk_ble_active_profile_index();
        bool is_connected = zmk_ble_active_profile_is_connected();
        bool is_open = zmk_ble_active_profile_is_open();

        struct led_rgb active_color;
        uint32_t interval_ms;

        if (is_connected) {
            /* Currently connected: flash green */
            active_color = COLOR_GREEN;
            interval_ms = CONFIG_ZMK_STATUS_LEDS_FAST_BLINK_MS;
        } else if (is_open) {
            /* Unoccupied / pairing mode: flash blue fast */
            active_color = COLOR_BLUE;
            interval_ms = CONFIG_ZMK_STATUS_LEDS_FAST_BLINK_MS;
        } else {
            /* Occupied / waiting for connection: blink yellow slowly */
            active_color = COLOR_YELLOW;
            interval_ms = CONFIG_ZMK_STATUS_LEDS_SLOW_BLINK_MS;
        }

        if (active_idx < bt_led_count) {
            set_pixel_safe(bt_leds[active_idx], blink_phase_on ? active_color : COLOR_OFF);
        }

        blink_phase_on = !blink_phase_on;
        flush_strip();
        k_work_reschedule(&status_tick_work, K_MSEC(interval_ms));
#else
        flush_strip();
        k_work_reschedule(&status_tick_work, K_MSEC(CONFIG_ZMK_STATUS_LEDS_DURATION_MS));
#endif
    }
}

void zmk_status_leds_show_battery(void) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    k_work_cancel_delayable(&status_tick_work);
    acquire_strip();
    current_mode = STATUS_MODE_BATTERY;
    display_end_time_ms = k_uptime_get() + CONFIG_ZMK_STATUS_LEDS_DURATION_MS;

    /* Delay 20ms so any pending underglow_off_work finishes before we draw */
    k_work_reschedule(&status_tick_work, K_MSEC(20));
#else
    LOG_WRN("CONFIG_ZMK_BATTERY_REPORTING is not enabled");
#endif
}

void zmk_status_leds_show_bt(void) {
    k_work_cancel_delayable(&status_tick_work);
    acquire_strip();
    current_mode = STATUS_MODE_BT;
    blink_phase_on = true;
    display_end_time_ms = k_uptime_get() + CONFIG_ZMK_STATUS_LEDS_DURATION_MS;

    /* Delay 20ms so any pending underglow_off_work finishes before we draw */
    k_work_reschedule(&status_tick_work, K_MSEC(20));
}

void zmk_status_leds_discover(void) {
    k_work_cancel_delayable(&status_tick_work);
    acquire_strip();
    current_mode = STATUS_MODE_DISCOVER;
    discover_idx = 0;

    /* Delay 20ms so any pending underglow_off_work finishes before we draw */
    k_work_reschedule(&status_tick_work, K_MSEC(20));
}

#if IS_ENABLED(CONFIG_ZMK_BLE) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
static int status_leds_ble_listener_cb(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    zmk_status_leds_show_bt();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_leds_ble_listener, status_leds_ble_listener_cb);
ZMK_SUBSCRIPTION(status_leds_ble_listener, zmk_ble_active_profile_changed);
#endif

static int status_leds_init(void) {
    k_work_init_delayable(&status_tick_work, status_tick_handler);
    return 0;
}

SYS_INIT(status_leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else
#warning "CONFIG_ZMK_STATUS_LEDS is enabled, but no 'zmk,status-leds' node was found in the Devicetree."
void zmk_status_leds_show_battery(void) {}
void zmk_status_leds_show_bt(void) {}
void zmk_status_leds_discover(void) {}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
