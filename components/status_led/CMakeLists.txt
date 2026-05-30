# REQUIRES is unconditional on purpose: component requirements are resolved before
# Kconfig, so a CONFIG_-gated REQUIRES would be silently dropped. The led_strip
# usage in status_led.c is #if CONFIG_STATUS_LED_WS2812 gated; when the feature is
# off the (still-fetched) led_strip component is simply unused.
set(dependencies "serial_handler" "led_strip")

idf_component_register(SRCS "status_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${dependencies})
