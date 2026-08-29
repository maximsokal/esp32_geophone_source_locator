idf_component_register(
    SRCS
        "object_service.c"
        "object_service_shutdown.c"
        "object_service_state.c"
        "object_service_submit.c"
        "object_service_worker.c"
        "object_service_operations.c"
        "object_service_mutations.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        object_format
        freertos
        log

    PRIV_REQUIRES
        object_store
)
