idf_component_register(
    SRCS
        "seismic_acquisition.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        geophone_adc_test
        seismic_test_generator
        log
)
