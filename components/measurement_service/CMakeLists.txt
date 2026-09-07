idf_component_register(
    SRCS
        "measurement_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        object_service
        seismic_recorder
        freertos
        log
)
