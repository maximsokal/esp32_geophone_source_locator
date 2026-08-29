idf_component_register(
    SRCS
        "ui.c"
        "ui_lvgl.c"
        "ui_event_task.c"
        "ui_screen.c"
        "ui_home.c"
        "ui_measurements.c"
        "ui_objects.c"
        "ui_storage.c"
        "ui_wifi.c"
        "ui_system.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_event_bus
        app_config
        display
        keypad
        device_status
        wifi_manager
        object_service
        storage_service
        seismic_acquisition
        esp_lvgl_port
        lvgl
        esp_system
        esp_timer
        freertos
        log
)
