idf_component_register(
    SRCS
        "object_recovery.c"
        "object_recovery_worker.c"
        "object_recovery_scan.c"
        "object_recovery_fs.c"
        "object_recovery_metadata_validate.c"
        "object_recovery_metadata.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        object_format
        object_store
        storage_manager
        seismic_recorder
        freertos
        json
        esp_timer
        esp_system
        heap
        log
)
