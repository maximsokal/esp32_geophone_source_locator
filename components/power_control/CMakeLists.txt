idf_component_register(
    SRCS
        "power_control.c"
        "power_output.c"
        "power_button_input.c"
        "power_button_monitor.c"
        "power_button_monitor_state.c"
        "power_button_monitor_wait.c"
        "power_button_monitor_task.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        board_config
        esp_driver_gpio
        freertos
        log
)
