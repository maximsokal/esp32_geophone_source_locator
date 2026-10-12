idf_component_register(
    SRCS
        "nrf_topology.c"
        "nrf_topology_bus_recovery.c"
        "nrf_sensor_transport.c"
        "nrf_node_link_status_policy.c"
        "nrf_log_collector.c"
        "nrf_topology_leveling_policy.c"
        "nrf_topology_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        device_status
        esp_hw_support
        esp_timer
        freertos
        i2c_bus
        log
        nrf_cs_authority
        nrf_cs_shared
        nrf_geometry_solver
        nrf_upstream_protocol
        sensor_bus
        tca9548a_mux
)
