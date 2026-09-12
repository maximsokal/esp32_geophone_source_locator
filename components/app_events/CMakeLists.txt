idf_component_register(
    SRCS
        "app_events.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        esp_event
        log
)
