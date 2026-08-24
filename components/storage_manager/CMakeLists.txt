idf_component_register(
    SRCS
        "storage_manager.c"
        "storage_manager_erase.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        freertos
        log
)