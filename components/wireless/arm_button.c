/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "arm_button.h"
#include "sdkconfig.h"
#if CONFIG_WIRELESS_SERIAL
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "arm_button";
static int s_gpio;
static arm_button_cb_t s_cb;

#define POLL_MS         10
#define DEBOUNCE_MS     20
#define DBLCLICK_MS     400

static void task(void *arg)
{
    (void) arg;
    int last = 1, stable = 1;                 /* active-low: 1 == released */
    int64_t last_change_us = 0, first_release_us = 0;
    int clicks = 0;
    while (1) {
        const int raw = gpio_get_level(s_gpio);    /* pressed == 0 */
        const int64_t now = esp_timer_get_time();
        if (raw != last) {
            last = raw;
            last_change_us = now;
        } else if (raw != stable && (now - last_change_us) > DEBOUNCE_MS * 1000) {
            stable = raw;
            if (stable == 0) {                /* debounced press edge */
                if (clicks == 1 && (now - first_release_us) < DBLCLICK_MS * 1000) {
                    clicks = 0;
                    ESP_LOGI(TAG, "double-click -> arm");
                    if (s_cb) {
                        s_cb();
                    }
                }
            } else {                          /* debounced release edge */
                if (clicks == 0) {
                    clicks = 1;
                    first_release_us = now;
                }
            }
        }
        if (clicks == 1 && (now - first_release_us) > DBLCLICK_MS * 1000) {
            clicks = 0;                        /* double-click window expired */
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void arm_button_init(int gpio, arm_button_cb_t on_double_click)
{
    s_gpio = gpio;
    s_cb = on_double_click;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    xTaskCreate(task, "arm_button", 2560, NULL, 4, NULL);
}
#endif
