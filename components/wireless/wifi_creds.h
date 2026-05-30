/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#define WIFI_CREDS_MAX     8
#define WIFI_SSID_MAXLEN   32
#define WIFI_PWD_MAXLEN    64
#define WIFI_TXT_MAXLEN    4096

typedef enum { WIFI_FAIL_NONE = 0, WIFI_FAIL_NOT_FOUND, WIFI_FAIL_REFUSED } wifi_fail_t;
typedef struct {
    char ssid[WIFI_SSID_MAXLEN + 1];
    char pwd[WIFI_PWD_MAXLEN + 1];
} wifi_cred_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Pure: parse text into creds[], return count (<= max). No IDF deps. */
size_t wifi_creds_parse(const char *buf, size_t len, wifi_cred_t *creds, size_t max);
/* Pure: true if buf is empty or only whitespace/CR/LF. */
bool   wifi_creds_is_blank(const char *buf, size_t len);
/* Pure: write the fill-in template into out, return length (< outsize). */
size_t wifi_creds_template(char *out, size_t outsize);
/* Pure: rebuild file text: working creds on top, failed ones under a
 * "# Failed credentials" header with annotations. results[i] pairs with creds[i]. */
size_t wifi_creds_rewrite(char *out, size_t outsize,
                          const wifi_cred_t *creds, const wifi_fail_t *results, size_t n);

#ifndef WIFI_CREDS_HOST_TEST
#include "esp_err.h"
/* LittleFS-backed: base path "/cfg", partition label "storage", file "wifi.txt". */
esp_err_t wifi_creds_fs_init(void);
esp_err_t wifi_creds_fs_read(char *buf, size_t bufsize, size_t *out_len);
esp_err_t wifi_creds_fs_write(const char *buf, size_t len);
#endif

#ifdef __cplusplus
}
#endif
