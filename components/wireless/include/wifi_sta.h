/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read creds, connect candidates in order; on success start mDNS + rfc2217. */
void wifi_sta_start(void);
/* Re-read creds and try again (after a live wifi.txt edit). */
void wifi_sta_retry(void);
bool wifi_sta_connected(void);

#ifdef __cplusplus
}
#endif
