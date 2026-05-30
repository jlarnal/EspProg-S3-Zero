/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_creds.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Extract a key="value" from one already-comment-stripped, trimmed line.
 * Returns 1 and copies value (truncated to vmax) if line is key="...". */
static int match_kv(const char *line, const char *key, char *val, size_t vmax)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) {
        return 0;
    }
    const char *p = line + klen;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '=') {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return 0;
    }
    p++;
    const char *end = strrchr(p, '"');     /* last quote on the line */
    if (!end || end < p) {
        return 0;
    }
    size_t n = (size_t)(end - p);
    if (n > vmax) {
        n = vmax;
    }
    memcpy(val, p, n);
    val[n] = '\0';
    return 1;
}

/* Copy one LF-delimited line into out (strip trailing CR), drop the comment:
 * everything from the first '#' that is OUTSIDE double quotes. Then trim ends. */
static void clean_line(const char *src, size_t len, char *out, size_t omax)
{
    int in_quotes = 0;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < omax; i++) {
        char ch = src[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '"') {
            in_quotes = !in_quotes;
        }
        if (ch == '#' && !in_quotes) {
            break;   /* comment start */
        }
        out[o++] = ch;
    }
    /* right-trim */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) {
        o--;
    }
    out[o] = '\0';
    /* left-trim */
    size_t s = 0;
    while (out[s] == ' ' || out[s] == '\t') {
        s++;
    }
    if (s) {
        memmove(out, out + s, strlen(out + s) + 1);
    }
}

size_t wifi_creds_parse(const char *buf, size_t len, wifi_cred_t *creds, size_t max)
{
    size_t count = 0;
    int have_pending = 0;
    char pending_ssid[WIFI_SSID_MAXLEN + 1] = {0};
    char line[256], val[WIFI_PWD_MAXLEN + 1];

    size_t i = 0;
    while (i < len && count < max) {
        size_t j = i;
        while (j < len && buf[j] != '\n') {
            j++;
        }
        clean_line(buf + i, j - i, line, sizeof(line));
        i = (j < len) ? j + 1 : j;

        if (line[0] == '\0') {
            continue;
        }
        if (match_kv(line, "SSID", val, WIFI_SSID_MAXLEN)) {
            strncpy(pending_ssid, val, sizeof(pending_ssid) - 1);
            pending_ssid[sizeof(pending_ssid) - 1] = '\0';
            have_pending = 1;                     /* last SSID wins */
        } else if (match_kv(line, "PWD", val, WIFI_PWD_MAXLEN)) {
            if (have_pending) {
                strncpy(creds[count].ssid, pending_ssid, WIFI_SSID_MAXLEN);
                creds[count].ssid[WIFI_SSID_MAXLEN] = '\0';
                strncpy(creds[count].pwd, val, WIFI_PWD_MAXLEN);
                creds[count].pwd[WIFI_PWD_MAXLEN] = '\0';
                count++;
                have_pending = 0;                 /* first PWD wins; ignore until next SSID */
            }
        }
    }
    return count;
}

bool wifi_creds_is_blank(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)buf[i])) {
            return false;
        }
    }
    return true;
}

static const char TEMPLATE_TXT[] =
    "# EspProg-S3 WiFi credentials.\n"
    "# Uncomment and fill the lines below (remove the leading '#'), then save.\n"
    "# Add more SSID/PWD pairs to list fallback networks; tried top to bottom.\n"
    "# Use PWD=\"\" for an open network.\n"
    "#\n"
    "#SSID=\"your network name\"\n"
    "#PWD=\"your password\"\n";

size_t wifi_creds_template(char *out, size_t outsize)
{
    size_t n = sizeof(TEMPLATE_TXT) - 1;
    if (n >= outsize) {
        n = outsize - 1;
    }
    memcpy(out, TEMPLATE_TXT, n);
    out[n] = '\0';
    return n;
}

size_t wifi_creds_rewrite(char *out, size_t outsize,
                          const wifi_cred_t *creds, const wifi_fail_t *results, size_t n)
{
    size_t o = 0;
#define APPENDF(...) do { \
        int _w = snprintf(out + o, outsize - o, __VA_ARGS__); \
        if (_w > 0 && (size_t)_w < outsize - o) { o += (size_t)_w; } \
    } while (0)
    /* working / untried first */
    for (size_t i = 0; i < n; i++) {
        if (results[i] == WIFI_FAIL_NONE) {
            APPENDF("SSID=\"%s\"\nPWD=\"%s\"\n\n", creds[i].ssid, creds[i].pwd);
        }
    }
    /* failed section */
    int any_failed = 0;
    for (size_t i = 0; i < n; i++) {
        if (results[i] != WIFI_FAIL_NONE) {
            any_failed = 1;
            break;
        }
    }
    if (any_failed) {
        APPENDF("# Failed credentials\n");
        for (size_t i = 0; i < n; i++) {
            if (results[i] == WIFI_FAIL_NOT_FOUND) {
                APPENDF("SSID=\"%s\" # <-- not found\nPWD=\"%s\"\n\n", creds[i].ssid, creds[i].pwd);
            } else if (results[i] == WIFI_FAIL_REFUSED) {
                APPENDF("SSID=\"%s\"\nPWD=\"%s\" # <-- refused\n\n", creds[i].ssid, creds[i].pwd);
            }
        }
    }
#undef APPENDF
    return o;
}

#ifndef WIFI_CREDS_HOST_TEST
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include "esp_littlefs.h"
#include "esp_log.h"

#define CFG_BASE   "/cfg"
#define CFG_PART   "storage"
#define CFG_FILE   CFG_BASE "/wifi.txt"
static const char *FTAG = "wifi_creds";

esp_err_t wifi_creds_fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_BASE,
        .partition_label = CFG_PART,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(FTAG, "littlefs register failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_creds_fs_read(char *buf, size_t bufsize, size_t *out_len)
{
    *out_len = 0;
    FILE *f = fopen(CFG_FILE, "rb");
    if (!f) {
        return ESP_OK;   /* missing == empty; caller writes a template */
    }
    size_t n = fread(buf, 1, bufsize - 1, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return ESP_OK;
}

esp_err_t wifi_creds_fs_write(const char *buf, size_t len)
{
    FILE *f = fopen(CFG_FILE, "wb");
    if (!f) {
        ESP_LOGE(FTAG, "open %s for write failed", CFG_FILE);
        return ESP_FAIL;
    }
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    return (w == len) ? ESP_OK : ESP_FAIL;
}
#endif /* CONFIG_WIRELESS_SERIAL */
#endif /* WIFI_CREDS_HOST_TEST */
