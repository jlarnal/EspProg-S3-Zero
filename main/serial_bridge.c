/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "serial_bridge.h"
#include "serial_handler.h"
#include "tusb_config.h"
#include "tusb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "util.h"
#include "debug_probe.h"

#define USB_SEND_RINGBUFFER_SIZE (4 * 1024)
/* If the CDC FIFO can't be drained for this long the host isn't reading; give up
 * on the current chunk rather than stalling the UART/bridge tasks forever. Under
 * normal operation the host drains within a couple of USB frames and this never
 * trips, so the path is effectively lossless (which esptool's SLIP framing needs). */
#define USB_TX_STALL_GIVEUP_TICKS 100   /* ~1s at the default 100Hz tick */
#define USB_RB_FULL_GIVEUP_TRIES  20    /* ~1s of 50ms ringbuffer-send retries */

static const char *TAG = "serial_bridge";

static RingbufHandle_t usb_sendbuf;
static esp_timer_handle_t state_change_timer;

// Transport data received callback - called by serial handler when data arrives
static void transport_data_received_callback(const uint8_t *data, size_t len)
{
    // With the new API, the callback is only called when bridge mode is active
    // (i.e., when flashing is not in progress), so we don't need to check mode
    ESP_LOGD(TAG, "Transport -> USB ringbuffer (%zu bytes)", len);
    ESP_LOG_BUFFER_HEXDUMP("Transport -> USB", data, len, ESP_LOG_DEBUG);

    // Queue received transport data for USB CDC. Block (back-pressuring the UART
    // RX task) instead of dropping: a dropped byte corrupts esptool's SLIP frame
    // and aborts the flash. During a session the target only emits in response to
    // a host command, so this never backs up unbounded. Only give up if the host
    // genuinely stops draining for ~1s (port closed / wedged).
    for (int tries = 0; xRingbufferSend(usb_sendbuf, data, len, pdMS_TO_TICKS(50)) != pdTRUE; tries++) {
        if (tries >= USB_RB_FULL_GIVEUP_TRIES) {
            ESP_LOGW(TAG, "USB send ringbuffer full >1s, dropping %zu bytes", len);
            return;
        }
    }
}

static void usb_sender_task(void *pvParameters)
{
    while (1) {
        size_t ringbuf_received;
        uint8_t *buf = xRingbufferReceiveUpTo(usb_sendbuf, &ringbuf_received, pdMS_TO_TICKS(100),
                                              CFG_TUD_CDC_TX_BUFSIZE);
        if (!buf) {
            ESP_LOGD(TAG, "usb_sender_task: nothing to send");
            continue;
        }

        uint8_t int_buf[CFG_TUD_CDC_TX_BUFSIZE];
        memcpy(int_buf, buf, ringbuf_received);
        vRingbufferReturnItem(usb_sendbuf, (void *) buf);

        // Lossless write: push the whole chunk into the CDC TX FIFO, flushing and
        // waiting for free space as needed. tud_cdc_write_available() paces us to
        // however fast tud_task() drains the endpoint; we never clear/drop bytes
        // (the old semaphore handshake could time out and discard mid-chunk, which
        // is what broke sustained transfers like esptool flashing).
        size_t off = 0, stalls = 0;
        while (off < ringbuf_received) {
            const uint32_t avail = tud_cdc_write_available();
            if (avail == 0) {
                tud_cdc_write_flush();
                if (++stalls > USB_TX_STALL_GIVEUP_TICKS) {
                    ESP_LOGW(TAG, "CDC TX FIFO stalled >1s, dropping %zu bytes", ringbuf_received - off);
                    break;
                }
                vTaskDelay(1);
                continue;
            }
            stalls = 0;
            size_t n = ringbuf_received - off;
            if (n > avail) {
                n = avail;
            }
            off += tud_cdc_write(int_buf + off, n);
            tud_cdc_write_flush();
        }
    }
    vTaskDelete(NULL);
}

void tud_cdc_rx_cb(const uint8_t itf)
{
    uint8_t buf[CFG_TUD_CDC_RX_BUFSIZE];

    const uint32_t rx_size = tud_cdc_n_read(itf, buf, CFG_TUD_CDC_RX_BUFSIZE);
    if (rx_size > 0) {
        ESP_LOGD(TAG, "USB CDC -> Transport (%" PRIu32 " bytes)", rx_size);
        ESP_LOG_BUFFER_HEXDUMP("USB CDC -> Transport", buf, rx_size, ESP_LOG_DEBUG);

        // Claim/refresh USB ownership of the UART; only drive the target if we own
        // it (a network RFC2217 session may currently hold the lock).
        serial_handler_mark_usb_activity();
        if (serial_handler_owner() == SERIAL_OWNER_USB) {
            // Send to transport (could be UART, SPI, I2C, etc.)
            serial_handler_send_data(buf, rx_size);
        }
    } else {
        ESP_LOGW(TAG, "tud_cdc_rx_cb receive error");
    }
}

void tud_cdc_line_coding_cb(const uint8_t itf, cdc_line_coding_t const *p_line_coding)
{
    if (serial_handler_set_baudrate(p_line_coding->bit_rate) != ESP_OK) {
        ESP_LOGE(TAG, "Could not set the baudrate to %" PRIu32, p_line_coding->bit_rate);
        eub_abort();
    }
}

void tud_cdc_line_state_cb(const uint8_t itf, const bool dtr, const bool rts)
{
    // The following transformation of DTR & RTS signals to BOOT & RST is done based on auto reset circutry shown in
    // schematics of ESP boards.

    // defaults for ((dtr && rts) || (!dtr && !rts))
    bool rst = true;
    bool boot = true;

    if (!dtr && rts) {
        rst = false;
        boot = true;
    } else if (dtr && !rts) {
        rst = true;
        boot = false;
    }

    esp_timer_stop(state_change_timer);  // maybe it is not started so not check the exit value

    if (dtr & rts) {
        // The assignment of BOOT=1 and RST=1 is postponed and it is done only if no other state change occurs in time
        // period set by the timer.
        // This is a patch for Esptool. Esptool generates DTR=0 & RTS=1 followed by DTR=1 & RTS=0. However, a callback
        // with DTR = 1 & RTS = 1 is received between. This would prevent to put the target chip into download mode.
        ESP_ERROR_CHECK(esp_timer_start_once(state_change_timer, 10 * 1000 /*us*/));

    } else {
        ESP_LOGI(TAG, "DTR = %d, RTS = %d -> BOOT = %d, RST = %d", dtr, rts, boot, rst);

        serial_handler_set_boot_reset_pins(boot, rst);

        if (!rst) {
            const uint32_t default_baud = 115200;
            if (serial_handler_set_baudrate(default_baud) != ESP_OK) {
                eub_abort();
            }
        }

        // On ESP32, TDI jtag signal is on GPIO12, which is also a strapping pin that determines flash voltage.
        // If TDI is high when ESP32 is released from external reset, the flash voltage is set to 1.8V, and the chip will fail to boot.
        // As a solution, MTDI signal forced to be low when RST is about to go high.
        if (boot) {
            debug_probe_handle_esp32_tdi_bootstrapping(!rst);
        }
    }
}

static void state_change_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "BOOT = 1, RST = 1");
    serial_handler_set_boot_reset_pins(true, true); // BOOT=1, RST=1 (not in reset)
}

static void init_state_change_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = state_change_timer_cb,
        .name = "serial_bridge_state_change"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &state_change_timer));
}

esp_err_t serial_bridge_init(void)
{
    // Create ring buffer for USB sending
    usb_sendbuf = xRingbufferCreate(USB_SEND_RINGBUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!usb_sendbuf) {
        ESP_LOGE(TAG, "Cannot create ringbuffer for USB sender");
        return ESP_ERR_NO_MEM;
    }

    // Register callback for transport data
    esp_err_t ret = serial_handler_register_data_callback(transport_data_received_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register transport data callback");
        return ret;
    }

    // Initialize state change timer
    init_state_change_timer();

    // Start USB sender task
    xTaskCreate(usb_sender_task, "usb_sender_task", 4 * 1024, NULL, SERIAL_HANDLER_TASK_PRI, NULL);

    ESP_LOGI(TAG, "Serial bridge initialized");
    return ESP_OK;
}
