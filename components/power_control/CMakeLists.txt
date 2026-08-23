idf_component_register(
    SRCS
        "power_control.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board_config
        esp_driver_gpio
        freertos
        log
)