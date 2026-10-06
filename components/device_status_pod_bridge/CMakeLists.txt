idf_component_register(
    SRCS
        "app_pod_status_barometer_policy.c"
        "app_pod_status_inventory_policy.c"
        "app_pod_status_local_position_policy.c"
        "app_pod_status_state_policy.c"
        "app_pod_status_bridge.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        device_status
        freertos
        hw_inventory
        log
        nrf_topology_service
        object_store

    PRIV_REQUIRES
        heap
)
