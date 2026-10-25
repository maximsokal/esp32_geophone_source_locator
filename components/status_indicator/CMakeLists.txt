idf_component_register(
    SRCS
        "status_indicator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_events
        board_config
        device_status
        esp_event
        freertos
        hw_inventory
        led_indicator
        log
)
