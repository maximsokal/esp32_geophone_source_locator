idf_component_register(
    SRCS
        "measurement_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        object_format
        object_service
        seismic_recorder
        runtime_observability
        storage_manager
        freertos
        heap
        log
)
