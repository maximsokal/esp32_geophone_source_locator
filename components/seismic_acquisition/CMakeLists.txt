idf_component_register(
    SRCS
        "seismic_acquisition.c"
        "seismic_source_registry.c"
        "seismic_source_common.c"
        "seismic_source_test.c"
        "seismic_source_esp_adc.c"
        "seismic_source_ad7606.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        ad7606_acquisition
        ad7606_driver
        app_config
        device_status
        geophone_adc_test
        seismic_buffer
        seismic_test_generator
        log
)
