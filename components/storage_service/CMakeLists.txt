idf_component_register(
    SRCS
        "storage_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        sd_card
        storage_manager
        object_store
        object_service
        power_control
        freertos
        log
)