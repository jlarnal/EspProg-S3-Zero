/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wireless.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include <string.h>
#include "wifi_creds.h"
#include "wifi_sta.h"
#include "arm_button.h"
#include "esp_log.h"

static const char *TAG = "wireless";
static bool s_armed = false;

/* Canonical WIFI.TXT cache. The wireless component owns it; main/msc.c reads it
 * via wireless_wifi_txt() to serve the MSC mirror (main depends on wireless, not
 * the reverse). */
static char   s_txt[WIFI_TXT_MAXLEN];
static size_t s_txt_len = 0;

static void cache_set(const char *buf, size_t len, bool persist)
{
    if (len > sizeof(s_txt)) {
        len = sizeof(s_txt);
    }
    memcpy(s_txt, buf, len);
    s_txt_len = len;
    if (persist) {
        wifi_creds_fs_write(s_txt, len);
    }
}

static void load_initial(void)
{
    size_t len = 0;
    if (wifi_creds_fs_read(s_txt, sizeof(s_txt), &len) != ESP_OK) {
        len = 0;
    }
    s_txt_len = len;
    if (len == 0 || wifi_creds_is_blank(s_txt, len)) {
        len = wifi_creds_template(s_txt, sizeof(s_txt));
        s_txt_len = len;
        wifi_creds_fs_write(s_txt, len);
    }
}

static void on_double_click(void)
{
    wireless_arm();
}

void wireless_init(void)
{
#if CONFIG_WIRELESS_SELFTEST
    {
        wifi_cred_t c[WIFI_CREDS_MAX];
        const char *ref = "SSID=\"a\"\nSSID=\"b\"\nPWD=\"p\"\nPWD=\"q\"\n";
        size_t n = wifi_creds_parse(ref, strlen(ref), c, WIFI_CREDS_MAX);
        ESP_LOGI(TAG, "SELFTEST parse: %s",
                 (n == 1 && !strcmp(c[0].ssid, "b") && !strcmp(c[0].pwd, "p")) ? "PASS" : "FAIL");
    }
#endif
    if (wifi_creds_fs_init() != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS init failed; wireless unavailable");
        return;
    }
    load_initial();
    arm_button_init(CONFIG_WIRELESS_ARM_GPIO, on_double_click);
    ESP_LOGI(TAG, "wireless ready (disarmed) — double-click BOOT to arm");
}

bool wireless_is_armed(void)
{
    return s_armed;
}

void wireless_arm(void)
{
    if (s_armed) {
        return;
    }
    s_armed = true;
    ESP_LOGI(TAG, "arming WiFi + RFC2217");
    wifi_sta_start();
}

void wireless_wifi_txt(const char **buf, uint32_t *len)
{
    *buf = s_txt;
    *len = (uint32_t)s_txt_len;
}

void wireless_update_wifi_txt(const char *buf, size_t len)
{
    cache_set(buf, len, true);   /* firmware rewrite (e.g. failed-creds section) */
}

void wireless_on_creds_written(const char *buf, size_t len)
{
    cache_set(buf, len, true);   /* host wrote WIFI.TXT */
    if (s_armed) {
        wifi_sta_retry();        /* live edit re-triggers a connect attempt */
    }
}
#else
void wireless_init(void) {}
bool wireless_is_armed(void) { return false; }
void wireless_arm(void) {}
void wireless_wifi_txt(const char **b, uint32_t *l) { *b = ""; *l = 0; }
void wireless_update_wifi_txt(const char *b, size_t l) { (void)b; (void)l; }
void wireless_on_creds_written(const char *b, size_t l) { (void)b; (void)l; }
#endif
