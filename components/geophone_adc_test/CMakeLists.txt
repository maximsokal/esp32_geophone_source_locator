idf_component_register(
    SRCS
        "geophone_adc_test.c"
        "geophone_adc_setup.c"
        "geophone_adc_scale.c"
        "geophone_adc_worker.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        adc_service
        board_config
        seismic_buffer
        esp_adc
        esp_timer
        freertos
        log
)
