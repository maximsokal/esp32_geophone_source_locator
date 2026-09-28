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
        board_config
        ina3221_monitor
        runtime_observability
        seismic_buffer
        esp_timer
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_pcnt
        esp_rom
        heap
        freertos
        log
)
