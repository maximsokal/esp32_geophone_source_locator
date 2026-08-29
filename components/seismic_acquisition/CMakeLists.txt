idf_component_register(
    SRCS
        "seismic_acquisition.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        ad7606_acquisition
        geophone_adc_test
        seismic_test_generator
        log
)