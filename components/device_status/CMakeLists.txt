idf_component_register(
    SRCS
        "device_status.c"
        "device_status_updates.c"
        "device_status_pods.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        freertos
)