idf_component_register(
    SRCS
        "runtime_observability.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        esp_timer
        freertos
        heap
        log
)
