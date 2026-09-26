idf_component_register(
    SRCS
        "ina3221_driver.c"
        "ina3221_limits.c"
        "ina3221_low_battery.c"
        "ina3221_power_ux.c"
        "ina3221_soc.c"
        "ina3221_monitor.c"
        "ina3221_policy.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        app_events
        device_status
        freertos
        log
        main_barometer
        sensor_bus
)
