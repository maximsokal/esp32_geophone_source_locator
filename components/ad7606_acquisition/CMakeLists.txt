idf_component_register(
    SRCS
        "ad7606_acquisition.c"
        "ad7606_acquisition_timing.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        ad7606_driver
        runtime_observability
        seismic_buffer
        esp_timer
        freertos
        log
)
