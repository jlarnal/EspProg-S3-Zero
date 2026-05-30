set(dependencies freertos esp_system)

# Starting from esp-idf v5.3, the GPIO driver is moved to a separate component
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND dependencies "esp_driver_gpio")
else()
    list(APPEND dependencies "driver")
endif()

# eub_abort() can drive the WS2812 (status LED feature). This REQUIRES is
# unconditional on purpose: component requirements are resolved before Kconfig, so
# a CONFIG_-gated REQUIRES would be silently dropped. The C code is #if-gated, so
# when the feature is off this only adds an (unused) header path.
list(APPEND dependencies "status_led")

idf_component_register(SRCS "util.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${dependencies}
                      )
