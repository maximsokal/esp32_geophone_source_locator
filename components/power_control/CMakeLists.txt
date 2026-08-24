idf_component_register(
    SRCS
        "power_control.c"
        "power_button_monitor.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        board_config
        esp_driver_gpio
        freertos
        log
)
