idf_component_register(
    SRCS
        "workflow_core.c"
        "workflow_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        device_status
        esp_timer
        freertos
        main_barometer
        measurement_service
        nrf_topology_service
        object_service
        log
)
