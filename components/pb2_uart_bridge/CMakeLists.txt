idf_component_register(
    SRCS
        "pb2_uart_bridge.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        driver
        freertos
        seismic_buffer
        pb2_protocol
)
