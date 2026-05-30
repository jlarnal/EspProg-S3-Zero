/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_sta.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
/* Stub — real implementation lands in the WiFi STA manager task. */
void wifi_sta_start(void) {}
void wifi_sta_retry(void) {}
bool wifi_sta_connected(void) { return false; }
#endif
