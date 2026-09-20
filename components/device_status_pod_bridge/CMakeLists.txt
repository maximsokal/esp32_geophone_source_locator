idf_component_register(
    SRCS
        "app_pod_status_barometer_policy.c"
        "app_pod_status_bridge.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        device_status
        freertos
        log
        nrf_topology_service
)
