idf_component_register(
    SRCS
        "keypad.c"
        "keypad_button_adapter.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        adc_service
        app_events
        board_config
        button
        log
)
