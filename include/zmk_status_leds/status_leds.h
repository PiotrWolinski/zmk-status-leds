/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Trigger the battery level status display on the configured LED subarray.
 */
void zmk_status_leds_show_battery(void);

/**
 * @brief Trigger the Bluetooth connection/slot status display on the configured LEDs.
 */
void zmk_status_leds_show_bt(void);
