idf_component_register(
    SRCS
        "keypad.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board_config
        esp_adc
        freertos
        log
)