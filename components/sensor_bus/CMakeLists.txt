idf_component_register(
    SRCS
        "sensor_bus.c"
        "sensor_bus_lines.c"
        "i2c_line_recovery.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board_config
        freertos
        i2c_bus
        log
        esp_driver_gpio
        esp_driver_i2c
        esp_timer
)
