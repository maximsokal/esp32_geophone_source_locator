idf_component_register(
    SRCS
        "storage_service.c"
        "storage_service_operations.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        sd_card
        storage_manager
        object_store
        power_control
        freertos
        log
)
