/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_led.h"
#include "sdkconfig.h"

#if CONFIG_STATUS_LED_WS2812

#include <stdatomic.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "serial_handler.h"

static const char *TAG = "status_led";

#define LED_GPIO            CONFIG_STATUS_LED_GPIO
#define BRIGHTNESS_PCT      CONFIG_STATUS_LED_BRIGHTNESS

#define RENDER_PERIOD_MS    20    // ~50 Hz
#define BREATHE_PERIOD_MS   1400  // idle heartbeat period
#define BLIP_MS             90    // activity blip duration
#define DEBUG_HOLD_MS       250   // how long a debug blip holds the debug color
#define ERROR_BLINK_MS      150   // fatal red-blink half-period

static led_strip_handle_t s_strip = NULL;
static atomic_bool s_mounted = false;
static atomic_bool s_fatal = false;
static atomic_uint_least32_t s_tx_tick = 0;
static atomic_uint_least32_t s_rx_tick = 0;
static atomic_uint_least32_t s_dbg_tick = 0;

static inline uint8_t scale8(uint32_t v, uint32_t pct)
{
    return (uint8_t)(v * pct / 100);
}

// Push one color (desired *physical* R,G,B), scaled by the global brightness.
// The Waveshare ESP32-S3-Zero's onboard pixel is RGB-ordered, but the led_strip
// 2.x driver only emits GRB (which swaps the first two channels on the wire). We
// keep the driver at GRB and compensate by swapping red<->green here, so physical
// output matches the requested color.
static void put(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, scale8(g, BRIGHTNESS_PCT), scale8(r, BRIGHTNESS_PCT), scale8(b, BRIGHTNESS_PCT));
    led_strip_refresh(s_strip);
}

// True if the event at *t happened within `within` ticks of `now`.
static bool recent(atomic_uint_least32_t *t, uint32_t now, uint32_t within)
{
    const uint32_t v = atomic_load(t);
    return v != 0 && (now - v) < within;
}

static void render_task(void *arg)
{
    (void) arg;
    const uint32_t blip_ticks = pdMS_TO_TICKS(BLIP_MS);
    const uint32_t dbg_ticks = pdMS_TO_TICKS(DEBUG_HOLD_MS);
    const uint32_t breathe_ticks = pdMS_TO_TICKS(BREATHE_PERIOD_MS);

    while (1) {
        // On a fatal error, status_led_fatal() owns the pixel; step aside.
        if (!atomic_load(&s_fatal)) {
            const uint32_t now = xTaskGetTickCount();
            const bool flashing = serial_handler_is_flashing() || serial_handler_is_reset_active();
            const bool debug = recent(&s_dbg_tick, now, dbg_ticks);
            const bool mounted = atomic_load(&s_mounted);

            // Priority: FLASHING > DEBUG > READY(+blips) > IDLE.
            if (flashing) {
                // Solid blue with a gentle flicker.
                const uint8_t b = ((now / pdMS_TO_TICKS(70)) % 4 == 0) ? 150 : 255;
                put(0, 0, b);
            } else if (debug) {
                put(128, 0, 255); // purple
            } else if (mounted) {
                if (recent(&s_tx_tick, now, blip_ticks)) {
                    put(0, 0, 255);   // blue: bridge -> target
                } else if (recent(&s_rx_tick, now, blip_ticks)) {
                    put(0, 255, 255); // cyan: target -> bridge
                } else {
                    // Ready heartbeat: breathe green, never fully off.
                    const uint32_t pos = now % breathe_ticks;
                    const uint32_t half = breathe_ticks / 2;
                    const uint32_t tri = (pos < half) ? (pos * 100 / half)
                                                      : (100 - (pos - half) * 100 / half);
                    const uint32_t level = 15 + tri * 85 / 100; // 15..100
                    put(0, (uint8_t)(255 * level / 100), 0);
                }
            } else {
                put(0, 255, 0); // idle (no host yet): solid green
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RENDER_PERIOD_MS));
    }
}

esp_err_t status_led_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        // led_strip 2.x only emits GRB; this board's pixel is RGB-ordered, so the
        // red<->green swap is compensated in software (see put()).
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    led_strip_clear(s_strip);

    if (xTaskCreate(render_task, "status_led", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d (brightness %d%%)", LED_GPIO, BRIGHTNESS_PCT);
    return ESP_OK;
}

void status_led_set_mounted(bool mounted)
{
    atomic_store(&s_mounted, mounted);
}

void status_led_blip_tx(void)
{
    atomic_store(&s_tx_tick, (uint_least32_t) xTaskGetTickCount());
}

void status_led_blip_rx(void)
{
    atomic_store(&s_rx_tick, (uint_least32_t) xTaskGetTickCount());
}

void status_led_notify_debug(void)
{
    atomic_store(&s_dbg_tick, (uint_least32_t) xTaskGetTickCount());
}

void status_led_fatal(void)
{
    atomic_store(&s_fatal, true);
    if (s_strip == NULL) {
        return; // LED not initialized yet; nothing to show
    }
    // Let the render task observe s_fatal and stop touching the pixel.
    vTaskDelay(pdMS_TO_TICKS(RENDER_PERIOD_MS + 5));
    for (int i = 0; i < 8; ++i) {
        put(255, 0, 0); // red (red<->green swap handled in put())
        vTaskDelay(pdMS_TO_TICKS(ERROR_BLINK_MS));
        put(0, 0, 0);   // off
        vTaskDelay(pdMS_TO_TICKS(ERROR_BLINK_MS));
    }
}

#else // !CONFIG_STATUS_LED_WS2812 -- no-op stubs

esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set_mounted(bool mounted) { (void) mounted; }
void status_led_blip_tx(void) {}
void status_led_blip_rx(void) {}
void status_led_notify_debug(void) {}
void status_led_fatal(void) {}

#endif // CONFIG_STATUS_LED_WS2812
