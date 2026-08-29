idf_component_register(
    SRCS
        "seismic_acquisition.c"

    INCLUDE_DIRS
        "include"

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
