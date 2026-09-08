idf_component_register(
    SRCS
        "runtime_observability.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_timer
        freertos
)
