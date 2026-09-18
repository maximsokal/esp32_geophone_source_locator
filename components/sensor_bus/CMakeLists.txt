idf_component_register(
    SRCS
        "sensor_bus.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board_config
        freertos
        i2c_bus
)
