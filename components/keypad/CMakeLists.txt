idf_component_register(
    SRCS
        "keypad.c"
        "keypad_adc.c"
        "keypad_decode.c"
        "keypad_event_monitor.c"
        "keypad_event_worker.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        adc_service
        board_config
        freertos
        log
)
