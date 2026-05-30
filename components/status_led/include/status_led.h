/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Single-pixel WS2812 status indicator for the bridge. One RGB pixel conveys
// liveness, current mode, and activity. The mode (idle / ready / flashing /
// debug / error) is resolved internally by the render task from the mount state,
// serial_handler flash state, and the activity notifications below. Brightness is
// scaled by CONFIG_STATUS_LED_BRIGHTNESS.
//
// When CONFIG_STATUS_LED_WS2812 is disabled, all of these are no-op stubs so the
// rest of the firmware can call them unconditionally.

// Bring up the WS2812 (on CONFIG_STATUS_LED_GPIO) and start the render task.
esp_err_t status_led_init(void);

// USB mount state: mounted -> READY (solid green), unmounted -> IDLE (breathe green).
void status_led_set_mounted(bool mounted);

// Brief activity blips overlaid on the READY color.
void status_led_blip_tx(void); // bridge -> target  (blue)
void status_led_blip_rx(void); // target -> bridge  (cyan)

// Mark JTAG/SWD debug activity; latched briefly so the LED holds the debug color.
void status_led_notify_debug(void);

// Blocking red-blink for fatal errors (called from eub_abort, which then aborts).
void status_led_fatal(void);

#ifdef __cplusplus
}
#endif
