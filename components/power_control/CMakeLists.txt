idf_component_register(
    SRCS
        "power_control.c"
        "power_output.c"
        "power_button_adapter.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_events
        board_config
        button
        esp_driver_gpio
        freertos
        log
)
