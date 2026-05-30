/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*arm_button_cb_t)(void);

/* Watch 'gpio' (active-low, internal pull-up) for a double-click → on_double_click(). */
void arm_button_init(int gpio, arm_button_cb_t on_double_click);

#ifdef __cplusplus
}
#endif
