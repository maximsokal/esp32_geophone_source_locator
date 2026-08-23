idf_component_register(
    SRCS
        "ui.c"
        "ui_common.c"
        "ui_status_bar.c"
        "ui_home.c"
        "ui_measurements.c"
        "ui_storage.c"
        "ui_wifi.c"
        "ui_system.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        display
        keypad
        device_status
        app_config
        log
)