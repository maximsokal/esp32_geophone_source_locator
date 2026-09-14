idf_component_register(
    SRCS
        "tca9548a_mux.c"
        "tca9548a_mux_i2c_bus.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        i2c_bus
)
