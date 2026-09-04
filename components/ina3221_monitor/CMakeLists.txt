idf_component_register(
    SRCS
        "ina3221_driver.c"
        "ina3221_monitor.c"
        "ina3221_policy.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        board_config
        device_status
        esp_driver_i2c
        freertos
        log
)
