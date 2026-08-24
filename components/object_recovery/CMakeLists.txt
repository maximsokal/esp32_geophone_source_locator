idf_component_register(
    SRCS
        "object_recovery.c"
        "object_recovery_fs.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        storage_manager
        json
        log
)