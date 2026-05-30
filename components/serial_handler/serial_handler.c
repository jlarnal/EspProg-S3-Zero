/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <sys/param.h>

#include "sdkconfig.h"

#include "serial_handler.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#define KB(x) ((x) * 1024)

#define SLAVE_UART_NUM          UART_NUM_1
#define SLAVE_UART_BUF_SIZE     KB(2)
#define SLAVE_UART_DEFAULT_BAUD 115200
#define GPIO_BOOT CONFIG_SERIAL_HANDLER_GPIO_BOOT
#define GPIO_RST CONFIG_SERIAL_HANDLER_GPIO_RST
#define GPIO_RXD CONFIG_SERIAL_HANDLER_GPIO_RXD
#define GPIO_TXD CONFIG_SERIAL_HANDLER_GPIO_TXD

static const char *TAG = "serial_handler";

static serial_tx_notify_cb_t s_tx_callback = NULL;
static serial_rx_notify_cb_t s_rx_callback = NULL;

// Reset timer handle
static esp_timer_handle_t s_reset_timer = NULL;

static esp_err_t init_reset_timer(void);

// Transport state
typedef struct {
    transport_type_t type;
    bool is_initialized;

    // Atomic flashing state - true when flashing is in progress
    atomic_bool is_flashing;

    // UART specific
    QueueHandle_t uart_queue;

    // Data callback for bridge mode (only called when not flashing)
    transport_data_received_cb_t data_callback;
} transport_state_t;

static transport_state_t s_transport = {0};

// --- UART ownership lock: exactly one of USB-CDC / network drives the target ---
static volatile serial_owner_t s_owner = SERIAL_OWNER_NONE;
static volatile int64_t s_usb_active_until_us = 0;
#define USB_ACTIVE_WINDOW_US (1500 * 1000)   // USB keeps priority 1.5s after each CDC RX
static transport_data_received_cb_t s_net_cb = NULL;

void serial_handler_mark_usb_activity(void)
{
    s_usb_active_until_us = esp_timer_get_time() + USB_ACTIVE_WINDOW_US;
    if (s_owner == SERIAL_OWNER_NONE) {
        s_owner = SERIAL_OWNER_USB;
    }
}

serial_owner_t serial_handler_owner(void)
{
    return s_owner;
}

bool serial_handler_acquire(serial_owner_t who)
{
    if (who == SERIAL_OWNER_NET) {
        const bool usb_busy = esp_timer_get_time() < s_usb_active_until_us;
        if (serial_handler_is_flashing() || usb_busy || s_owner == SERIAL_OWNER_USB) {
            return false;
        }
        s_owner = SERIAL_OWNER_NET;
        return true;
    }
    s_owner = SERIAL_OWNER_USB;
    return true;
}

void serial_handler_release(serial_owner_t who)
{
    if (s_owner == who) {
        s_owner = SERIAL_OWNER_NONE;
    }
}

void serial_handler_register_net_data_callback(transport_data_received_cb_t cb)
{
    s_net_cb = cb;
}

#if CONFIG_SERIAL_HANDLER_TXD_RELEASE_WHEN_IDLE
// --- TxD pad ownership -------------------------------------------------------
// TxD is push-pull only while a programming/monitor session is active; when idle
// it is released to a true high-impedance state so a target that repurposes its
// RX pin at runtime (e.g. as a display control line) is not held by the dormant
// bridge. Release is driven by the target's reset-to-RUN transition (the reboot
// esptool issues at the end of a session), with an inactivity timeout as a
// backstop. TxD is never open-drain. See the txd-hiz-release-policy design note.
#define TXD_RUN_RELEASE_MARGIN_MS 30

typedef enum { TX_RELEASED, TX_ATTACHED } tx_owner_state_t;
static tx_owner_state_t s_tx_state = TX_RELEASED;
static SemaphoreHandle_t s_tx_mutex = NULL;
static esp_timer_handle_t s_tx_release_timer = NULL;
static atomic_bool s_target_running = true;

// Re-bind GPIO_TXD to the UART TX signal (push-pull). Caller must hold s_tx_mutex.
// uart_set_pin routes a non-IOMUX pin via the GPIO matrix and asserts the pad
// output-enable only after connecting the signal, so the pad snaps to the UART
// idle-HIGH mark rather than emitting a spurious start bit.
static void tx_attach_locked(void)
{
    if (s_tx_state == TX_ATTACHED) {
        return;
    }
    uart_set_pin(SLAVE_UART_NUM, GPIO_TXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    esp_rom_delay_us(50); // let the line settle at idle HIGH before the first start bit
    s_tx_state = TX_ATTACHED;
}

// Release GPIO_TXD to true Hi-Z. Clearing the pad output-enable disconnects the
// drive even though the UART out-signal stays routed in the matrix; no internal
// pull is applied so the target fully owns the line (idle bias, if a board needs
// it, is an external pull-up on the net). Caller must hold s_tx_mutex.
static void tx_release_locked(void)
{
    if (s_tx_state == TX_RELEASED) {
        return;
    }
    gpio_set_direction(GPIO_TXD, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_TXD, GPIO_FLOATING);
    s_tx_state = TX_RELEASED;
}

static void tx_arm_release_timer(uint32_t ms)
{
    if (s_tx_release_timer == NULL) {
        return;
    }
    esp_timer_stop(s_tx_release_timer); // harmless if not running
    esp_timer_start_once(s_tx_release_timer, (uint64_t)ms * 1000);
}

// Attach TxD if needed and (re)arm the inactivity backstop. Safe from any task.
static void tx_acquire(void)
{
    if (s_tx_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    tx_attach_locked();
    tx_arm_release_timer(CONFIG_SERIAL_HANDLER_TXD_IDLE_MS);
    xSemaphoreGive(s_tx_mutex);
}

static void tx_release_timer_cb(void *arg)
{
    (void) arg;
    if (s_tx_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    // Never release mid-flash, during the post-flash reset window, or before the
    // target has actually been released to run its application.
    if (atomic_load(&s_transport.is_flashing) ||
            serial_handler_is_reset_active() ||
            !atomic_load(&s_target_running)) {
        tx_arm_release_timer(CONFIG_SERIAL_HANDLER_TXD_IDLE_MS);
        xSemaphoreGive(s_tx_mutex);
        return;
    }
    // Make sure the shifter/FIFO is drained so a frame is never truncated; if not,
    // defer rather than cut the line mid-byte.
    if (uart_wait_tx_done(SLAVE_UART_NUM, pdMS_TO_TICKS(20)) != ESP_OK) {
        tx_arm_release_timer(CONFIG_SERIAL_HANDLER_TXD_IDLE_MS);
        xSemaphoreGive(s_tx_mutex);
        return;
    }
    tx_release_locked();
    xSemaphoreGive(s_tx_mutex);
}
#endif // CONFIG_SERIAL_HANDLER_TXD_RELEASE_WHEN_IDLE

void serial_handler_register_tx_activity_callback(serial_tx_notify_cb_t callback)
{
    s_tx_callback = callback;
}

void serial_handler_register_rx_activity_callback(serial_rx_notify_cb_t callback)
{
    s_rx_callback = callback;
}

static void serial_notify_tx_activity(bool active)
{
    if (s_tx_callback) {
        s_tx_callback(active);
    }
}

static void serial_notify_rx_activity(bool active)
{
    if (s_rx_callback) {
        s_rx_callback(active);
    }
}

// UART event handling task
static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t dtmp[SLAVE_UART_BUF_SIZE];

    while (1) {
        serial_notify_rx_activity(false);
        if (xQueueReceive(s_transport.uart_queue, &event, portMAX_DELAY)) {
            serial_notify_rx_activity(true);
            switch (event.type) {
            case UART_DATA:
                // Only call callback if not flashing and callback is registered
                if (!atomic_load(&s_transport.is_flashing) && (s_transport.data_callback || s_net_cb)) {
                    size_t buffered_len;
                    uart_get_buffered_data_len(SLAVE_UART_NUM, &buffered_len);
                    const int read = uart_read_bytes(SLAVE_UART_NUM, dtmp, MIN(buffered_len, SLAVE_UART_BUF_SIZE), portMAX_DELAY);
                    ESP_LOGD(TAG, "UART -> Bridge Callback (%d bytes)", read);
                    ESP_LOG_BUFFER_HEXDUMP("UART RX", dtmp, read, ESP_LOG_DEBUG);

                    // Route to the current UART owner: network sink if NET holds the
                    // lock, otherwise the registered (USB-CDC) data callback.
                    if (s_owner == SERIAL_OWNER_NET && s_net_cb) {
                        s_net_cb(dtmp, read);
                    } else if (s_transport.data_callback) {
                        s_transport.data_callback(dtmp, read);
                    }
                }
                // Note: When flashing, ESP loader will read data directly from UART
                break;
            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART FIFO overflow");
                uart_flush_input(SLAVE_UART_NUM);
                xQueueReset(s_transport.uart_queue);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART ring buffer full");
                uart_flush_input(SLAVE_UART_NUM);
                xQueueReset(s_transport.uart_queue);
                break;
            case UART_BREAK:
                ESP_LOGW(TAG, "UART RX break");
                break;
            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "UART parity error");
                break;
            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "UART frame error");
                break;
            default:
                ESP_LOGW(TAG, "UART event type: %d", event.type);
                break;
            }
            taskYIELD();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static esp_err_t init_uart_transport(void)
{
    const loader_esp32_config_t serial_conf = {
        .baud_rate = SLAVE_UART_DEFAULT_BAUD,
        .uart_port = SLAVE_UART_NUM,
        .uart_rx_pin = GPIO_RXD,
        .uart_tx_pin = GPIO_TXD,
        .rx_buffer_size = SLAVE_UART_BUF_SIZE * 2,
        .tx_buffer_size = 0,
        .uart_queue = &s_transport.uart_queue,
        .queue_size = 20,
        .reset_trigger_pin = GPIO_RST,
        .gpio0_trigger_pin = GPIO_BOOT,
    };

    if (loader_port_esp32_init(&serial_conf) == ESP_LOADER_SUCCESS) {
        // Enable pull up for RXD to avoid floating input
        ESP_RETURN_ON_ERROR(gpio_pullup_en(GPIO_RXD), TAG, "Failed to enable pull up for RXD");

        // Override BOOT/RST pins to open-drain so the bridge never actively drives them HIGH.
        // The internal pull-ups (and any external pull-ups on the target) define the idle HIGH level,
        // while other devices can still pull the lines LOW without bus contention.
        ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_BOOT, GPIO_MODE_OUTPUT_OD), TAG, "Failed to set boot pin to open-drain");
        ESP_RETURN_ON_ERROR(gpio_pullup_en(GPIO_BOOT), TAG, "Failed to enable pull up for BOOT");
        ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_BOOT, 1), TAG, "Failed to release boot pin");
        ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_RST, GPIO_MODE_OUTPUT_OD), TAG, "Failed to set reset pin to open-drain");
        ESP_RETURN_ON_ERROR(gpio_pullup_en(GPIO_RST), TAG, "Failed to enable pull up for RST");
        ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_RST, 1), TAG, "Failed to release reset pin");

#if CONFIG_SERIAL_HANDLER_TXD_RELEASE_WHEN_IDLE
        // Set up TxD pad ownership. loader_port_esp32_init left GPIO_TXD bound to
        // the UART as a push-pull output idling HIGH; start from the defined idle
        // state by releasing it to Hi-Z until the first session begins.
        s_tx_mutex = xSemaphoreCreateMutex();
        if (s_tx_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create TxD mutex");
            return ESP_ERR_NO_MEM;
        }
        const esp_timer_create_args_t tx_timer_args = {
            .callback = tx_release_timer_cb,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "serial_txd_release",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&tx_timer_args, &s_tx_release_timer), TAG, "Failed to create TxD release timer");
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        s_tx_state = TX_ATTACHED; // currently driven by the UART after init
        tx_release_locked();      // -> Hi-Z idle state
        xSemaphoreGive(s_tx_mutex);
#endif

        ESP_LOGI(TAG, "UART have been initialized");

        // Start UART event task
        xTaskCreate(uart_event_task, "uart_task", KB(8), NULL, SERIAL_HANDLER_TASK_PRI, NULL);

        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "loader_port_esp32_init failed");
        return ESP_FAIL;
    }
}

esp_err_t serial_handler_init(transport_type_t type)
{
    if (s_transport.is_initialized) {
        ESP_LOGW(TAG, "Comm handler already initialized");
        return ESP_OK;
    }

    s_transport.type = type;
    atomic_store(&s_transport.is_flashing, false);
    s_transport.data_callback = NULL;

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    switch (type) {
    case TRANSPORT_TYPE_UART:
        ret = init_uart_transport();
        break;
    default:
        ESP_LOGE(TAG, "Unknown transport type: %d", type);
        break;
    }

    if (ret == ESP_OK) {
        // Initialize reset timer
        ret = init_reset_timer();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize reset timer");
            return ret;
        }

        s_transport.is_initialized = true;
        ESP_LOGI(TAG, "Comm handler initialized for type %d", type);
    }

    return ret;
}

bool serial_handler_is_flashing(void)
{
    return atomic_load(&s_transport.is_flashing);
}

bool serial_handler_is_reset_active(void)
{
    return (s_reset_timer != NULL) && esp_timer_is_active(s_reset_timer);
}

esp_err_t serial_handler_register_data_callback(transport_data_received_cb_t callback)
{
    if (!s_transport.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_transport.data_callback = callback;
    ESP_LOGI(TAG, "Data callback registered");
    return ESP_OK;
}

esp_err_t serial_handler_send_data(const uint8_t *data, size_t len)
{
    if (!s_transport.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Sending %zu bytes", len);
    ESP_LOG_BUFFER_HEXDUMP("UART TX", data, len, ESP_LOG_DEBUG);

    switch (s_transport.type) {
    case TRANSPORT_TYPE_UART: {
#if CONFIG_SERIAL_HANDLER_TXD_RELEASE_WHEN_IDLE
        tx_acquire(); // ensure TxD is push-pull before the first byte; re-arm the backstop
#endif
        serial_notify_tx_activity(true);
        const int transferred = uart_write_bytes(SLAVE_UART_NUM, data, len);
        serial_notify_tx_activity(false);
        if (transferred != len) {
            ESP_LOGW(TAG, "uart_write_bytes transferred %d bytes only!", transferred);
            return ESP_ERR_INVALID_SIZE;
        }
        break;
    }
    default:
        ESP_LOGE(TAG, "Unknown transport type: %d", s_transport.type);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t serial_handler_set_baudrate(uint32_t baud)
{
    if (!s_transport.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    switch (s_transport.type) {
    case TRANSPORT_TYPE_UART: {
        esp_err_t result = uart_set_baudrate(SLAVE_UART_NUM, baud);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "UART baudrate set to %" PRIu32, baud);
        } else {
            ESP_LOGE(TAG, "Failed to set UART baudrate to %" PRIu32, baud);
        }
        return result;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

// Static variables to store flash operation state
static uint32_t s_flash_addr = 0;
static uint32_t s_flash_total_size = 0;
static bool s_flash_operation_started = false;

esp_err_t serial_handler_flash_connect(uint32_t baud_rate)
{
    if (!s_transport.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (atomic_load(&s_transport.is_flashing)) {
        ESP_LOGE(TAG, "Already flashing");
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_SERIAL_HANDLER_TXD_RELEASE_WHEN_IDLE
    tx_acquire(); // the loader is about to drive the target; keep TxD push-pull
#endif

    // Take exclusive access for flashing
    atomic_store(&s_transport.is_flashing, true);
    ESP_LOGI(TAG, "Flashing mode started - bridge callbacks suspended");

    // Set initial baudrate
    esp_err_t ret = serial_handler_set_baudrate(baud_rate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial baudrate");
        atomic_store(&s_transport.is_flashing, false);
        return ret;
    }

    // Connect to ESP chip
    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t loader_ret = esp_loader_connect(&connect_config);
    if (loader_ret != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "ESP LOADER connection failed: %d", loader_ret);
        atomic_store(&s_transport.is_flashing, false);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ESP LOADER connected successfully");
    return ESP_OK;
}

esp_err_t serial_handler_flash_change_baudrate(uint32_t chip_id, uint32_t new_baud)
{
    if (!atomic_load(&s_transport.is_flashing)) {
        ESP_LOGE(TAG, "Not in flashing mode");
        return ESP_ERR_INVALID_STATE;
    }

    // Change baudrate using ESP loader
    esp_loader_error_t loader_ret = esp_loader_change_transmission_rate(new_baud);
    if (loader_ret != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Failed to change ESP loader baudrate: %d", loader_ret);
        return ESP_FAIL;
    }

    // Update transport baudrate
    esp_err_t ret = serial_handler_set_baudrate(new_baud);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to change transport baudrate");
        return ret;
    }

    ESP_LOGI(TAG, "Baudrate changed to %" PRIu32, new_baud);
    return ESP_OK;
}

esp_err_t serial_handler_flash_start(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!atomic_load(&s_transport.is_flashing)) {
        ESP_LOGE(TAG, "Not in flashing mode");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Preparing flash operation at 0x%08" PRIx32 ", size %" PRIu32, addr, len);

    // Store flash operation parameters
    s_flash_addr = addr;
    s_flash_total_size = len;
    s_flash_operation_started = false;

    return ESP_OK;
}

esp_err_t serial_handler_flash_write(const uint8_t *data, uint32_t len)
{
    if (!atomic_load(&s_transport.is_flashing)) {
        ESP_LOGE(TAG, "Not in flashing mode");
        return ESP_ERR_INVALID_STATE;
    }

    // Use optimal block size for ESP loader (4KB seems like max for ROM flash)
    const uint32_t block_size = KB(4);

    // Start ESP loader operation if not already started
    if (!s_flash_operation_started) {
        ESP_LOGD(TAG, "Starting ESP loader flash operation at 0x%08" PRIx32 ", size %" PRIu32, s_flash_addr, s_flash_total_size);

        esp_loader_error_t ret = esp_loader_flash_start(s_flash_addr, s_flash_total_size, block_size);
        if (ret != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "esp_loader_flash_start failed: %d", ret);
            return ESP_FAIL;
        }
        s_flash_operation_started = true;
    }

    // Write data in chunks
    uint32_t remaining = len;
    const uint8_t *data_ptr = data;

    while (remaining > 0) {
        uint32_t bytes_to_write = MIN(remaining, block_size);
        ESP_LOGD(TAG, "Writing %" PRIu32 " bytes to flash", bytes_to_write);

        esp_loader_error_t ret = esp_loader_flash_write((void *)data_ptr, bytes_to_write);
        if (ret != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "esp_loader_flash_write failed: %d", ret);
            return ESP_FAIL;
        }

        remaining -= bytes_to_write;
        data_ptr += bytes_to_write;
    }

    return ESP_OK;
}

esp_err_t serial_handler_flash_read(uint8_t *data, uint32_t addr, uint32_t len)
{
    if (!atomic_load(&s_transport.is_flashing)) {
        ESP_LOGE(TAG, "Not in flashing mode");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Reading %" PRIu32 " bytes from flash at 0x%08" PRIx32, len, addr);

    esp_loader_error_t ret = esp_loader_flash_read(data, addr, len);
    if (ret != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "esp_loader_flash_read failed: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Reset timer callback - called when reset hold timeout expires
static void reset_timer_cb(void *arg)
{
    (void) arg;
    serial_handler_set_boot_reset_pins(true, true); // BOOT=1, RST=1 (target in normal boot)
    ESP_LOGD(TAG, "Target reset released");
}

static esp_err_t init_reset_timer(void)
{
    if (s_reset_timer != NULL) {
        return ESP_OK; // Already initialized
    }

    const esp_timer_create_args_t timer_args = {
        .callback = reset_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "serial_reset",
        .skip_unhandled_events = false,
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_reset_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create reset timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Reset timer initialized");
    return ESP_OK;
}

esp_err_t serial_handler_flash_finish(bool reboot)
{
    if (!atomic_load(&s_transport.is_flashing)) {
        ESP_LOGE(TAG, "Not in flashing mode");
        return ESP_ERR_INVALID_STATE;
    }

    // Reset flash operation state
    s_flash_operation_started = false;
    s_flash_addr = 0;
    s_flash_total_size = 0;

    // Release exclusive access
    atomic_store(&s_transport.is_flashing, false);
    ESP_LOGI(TAG, "Flashing mode finished - bridge callbacks resumed");

    // Perform non-blocking target reset only if reboot is requested
    if (reboot && s_reset_timer != NULL) {
        ESP_LOGD(TAG, "Starting target reset");
        serial_handler_set_boot_reset_pins(true, false); // BOOT=1, RST=0 (target in reset)
        esp_err_t timer_ret = esp_timer_start_once(s_reset_timer, SERIAL_FLASHER_RESET_HOLD_TIME_MS * 1000);
        if (timer_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start reset timer: %s", esp_err_to_name(timer_ret));
            // Continue anyway as the main flash operation was successful
        }
    } else if (reboot) {
        ESP_LOGW(TAG, "Reboot requested but reset timer not available");
    }

    return ESP_OK;
}

void serial_handler_set_boot_reset_pins(bool boot_pin, bool reset_pin)
{
    gpio_set_level(GPIO_BOOT, boot_pin);
    gpio_set_level(GPIO_RST, reset_pin);
    ESP_LOGD(TAG, "BOOT=%s, RST=%s", boot_pin ? "HIGH" : "LOW", reset_pin ? "HIGH" : "LOW");

#if CONFIG_SERIAL_HANDLER_TXD_RELEASE_WHEN_IDLE
    // Drive TxD attach/release off the target's boot/reset edges. This is the
    // single chokepoint both the CDC auto-reset path and the post-flash reset
    // funnel through. A reset released with BOOT low => target enters DOWNLOAD
    // (session active, keep TxD driven); released with BOOT high => target enters
    // RUN (the end-of-session reboot) => schedule TxD release so we go Hi-Z as the
    // app starts and may repurpose its RX pin. The inactivity timer is the backstop.
    static bool s_prev_rst = true; // assumed out of reset at power-up
    const bool rst_rising = (!s_prev_rst && reset_pin);
    s_prev_rst = reset_pin;

    if (!boot_pin || !reset_pin) {
        // A reset/boot manipulation is under way: a programming session is
        // (re)starting. Make sure TxD is driving before any SYNC byte.
        atomic_store(&s_target_running, false);
        tx_acquire();
    } else if (rst_rising) {
        atomic_store(&s_target_running, true);
        if (s_tx_mutex != NULL) {
            xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
            tx_arm_release_timer(TXD_RUN_RELEASE_MARGIN_MS);
            xSemaphoreGive(s_tx_mutex);
        }
    }
#endif
}
