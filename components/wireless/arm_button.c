/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "arm_button.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
/* Stub — real implementation lands in the double-click watcher task. */
void arm_button_init(int gpio, arm_button_cb_t cb) { (void)gpio; (void)cb; }
#endif
