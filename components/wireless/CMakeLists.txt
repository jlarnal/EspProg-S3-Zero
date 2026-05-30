idf_component_register(
    SRCS "wireless.c" "wifi_creds.c" "wifi_sta.c" "rfc2217.c" "arm_button.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_wifi esp_event esp_netif mdns littlefs nvs_flash
             lwip esp_driver_gpio esp_timer serial_handler status_led)
