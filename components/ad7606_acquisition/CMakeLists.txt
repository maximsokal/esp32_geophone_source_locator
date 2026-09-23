idf_component_register(
    SRCS
        "ad7606_acquisition.c"
        "ad7606_acquisition_timing.c"
        "ad7606_power_policy.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        ad7606_driver
        ina3221_monitor
        runtime_observability
        seismic_buffer
        esp_timer
        freertos
        log
)
