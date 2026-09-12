idf_component_register(
    SRCS
        "status_indicator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_events
        device_status
        esp_event
        freertos
        led_indicator
        log
)
