idf_component_register(
    SRCS
        "object_recovery.c"
        "object_recovery_scan.c"
        "object_recovery_fs.c"
        "object_recovery_metadata_validate.c"
        "object_recovery_metadata.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        object_format
        storage_manager
        json
        esp_timer
        log
)
