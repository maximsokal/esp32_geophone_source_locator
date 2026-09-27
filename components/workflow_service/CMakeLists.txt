idf_component_register(
    SRCS
        "workflow_core.c"
        "workflow_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        device_status
        esp_timer
        freertos
        heap
        json
        localization_job
        main_barometer
        measurement_service
        nrf_topology_service
        object_service
        object_store
        seismic_recorder
        log
)
