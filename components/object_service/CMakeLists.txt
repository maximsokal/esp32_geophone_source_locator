idf_component_register(
    SRCS
        "object_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        object_store
        freertos
        log
)