idf_component_register(
    SRCS
        "ad7606_acquisition.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        ad7606_driver
        seismic_buffer
        esp_timer
        freertos
        log
)
