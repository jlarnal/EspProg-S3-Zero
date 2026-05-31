/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_sta.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include <string.h>
#include <stdio.h>
#include "wifi_creds.h"
#include "wireless.h"
#include "rfc2217.h"
#include "status_led.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"

static const char *TAG = "wifi_sta";
static wifi_cred_t   s_creds[WIFI_CREDS_MAX];
static wifi_fail_t   s_results[WIFI_CREDS_MAX];
static size_t        s_n = 0, s_idx = 0;
static bool          s_connected = false;
static bool          s_started = false;

static void try_candidate(size_t i)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_creds[i].ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_creds[i].pwd, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = (s_creds[i].pwd[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "trying [%u/%u] SSID=\"%s\"", (unsigned)(i + 1), (unsigned)s_n, s_creds[i].ssid);
    status_led_set_wifi(LED_WIFI_CONNECTING);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

static void publish_results(void)
{
    /* static, NOT on the stack: this runs on the small system-event task and a
     * 4KB stack buffer would overflow it. Only ever called from the event handler
     * (serialized), so a single static buffer is safe. */
    static char out[WIFI_TXT_MAXLEN];
    size_t len = wifi_creds_rewrite(out, sizeof(out), s_creds, s_results, s_n);
    wireless_update_wifi_txt(out, len);   /* persist + refresh MSC mirror cache */
}

static void evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_n) {
            try_candidate(s_idx = 0);
        } else {
            ESP_LOGW(TAG, "no candidates in wifi.txt");
            status_led_set_wifi(LED_WIFI_FAILED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_connected) {                       /* drop after being connected -> reconnect */
            s_connected = false;
            status_led_set_wifi(LED_WIFI_CONNECTING);
            esp_wifi_connect();
            return;
        }
        s_results[s_idx] = (d->reason == WIFI_REASON_NO_AP_FOUND)
                           ? WIFI_FAIL_NOT_FOUND : WIFI_FAIL_REFUSED;
        ESP_LOGW(TAG, "candidate %u failed (reason %d)", (unsigned)s_idx, d->reason);
        if (++s_idx < s_n) {
            try_candidate(s_idx);
        } else {
            ESP_LOGW(TAG, "all candidates failed");
            status_led_set_wifi(LED_WIFI_FAILED);
            publish_results();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_results[s_idx] = WIFI_FAIL_NONE;
        status_led_set_wifi(LED_WIFI_CONNECTED);
        mdns_hostname_set(CONFIG_WIRELESS_MDNS_HOSTNAME);
        mdns_service_add(NULL, "_rfc2217", "_tcp", CONFIG_WIRELESS_RFC2217_PORT, NULL, 0);
        publish_results();                       /* record which earlier ones failed */
        rfc2217_start(CONFIG_WIRELESS_RFC2217_PORT);
    }
}

static void load_creds(void)
{
    const char *txt = NULL;
    uint32_t len = 0;
    wireless_wifi_txt(&txt, &len);               /* current cache (already loaded from FS) */
    s_n = wifi_creds_parse(txt, len, s_creds, WIFI_CREDS_MAX);
    for (size_t i = 0; i < WIFI_CREDS_MAX; i++) {
        s_results[i] = WIFI_FAIL_NONE;
    }
    s_idx = 0;
}

void wifi_sta_start(void)
{
    if (s_started) {
        wifi_sta_retry();
        return;
    }
    s_started = true;

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {   /* tolerate an existing default loop */
        ESP_ERROR_CHECK(r);
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    {
        /* Friendly DHCP/network hostname "Esp-Prog-<MAC LSW>" instead of the
         * default "espressif" (shows up in the router's client list). */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char host[24];
        snprintf(host, sizeof(host), "Esp-Prog-%02X%02X", mac[4], mac[5]);
        esp_netif_set_hostname(netif, host);
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, evt, NULL, NULL));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    load_creds();
    ESP_ERROR_CHECK(esp_wifi_start());           /* fires WIFI_EVENT_STA_START -> try_candidate */
}

void wifi_sta_retry(void)
{
    if (!s_started) {
        wifi_sta_start();
        return;
    }
    esp_wifi_disconnect();
    s_connected = false;
    load_creds();
    if (s_n) {
        try_candidate(s_idx = 0);
    }
}

bool wifi_sta_connected(void)
{
    return s_connected;
}
#endif
