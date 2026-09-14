idf_component_register(
    SRCS
        "nrf_topology.c"
        "nrf_topology_service.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        esp_hw_support
        freertos
        i2c_bus
        log
        nrf_upstream_protocol
        sensor_bus
        tca9548a_mux
)
