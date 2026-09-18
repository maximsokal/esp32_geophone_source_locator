idf_component_register(
    SRCS
        "nrf_emergency_stop_policy.c"
        "nrf_emergency_stop_monitor.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        freertos
        log
        nrf_topology_service
        seismic_recorder
)
