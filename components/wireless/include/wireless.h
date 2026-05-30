/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount FS, ensure wifi.txt (template if blank), start arm button. */
void wireless_init(void);
bool wireless_is_armed(void);
/* Idempotent: start wifi_sta (+ rfc2217 on connect). */
void wireless_arm(void);

/* Current WIFI.TXT content. main/msc.c calls this to serve the MSC mirror
 * (the wireless component owns the cache; main depends on wireless, not vice versa). */
void wireless_wifi_txt(const char **buf, uint32_t *len);
/* Firmware-side rewrite of WIFI.TXT (e.g. the failed-credentials section). */
void wireless_update_wifi_txt(const char *buf, size_t len);
/* Called by main/msc.c when WIFI.TXT is (over)written by the host. */
void wireless_on_creds_written(const char *buf, size_t len);

#ifdef __cplusplus
}
#endif
