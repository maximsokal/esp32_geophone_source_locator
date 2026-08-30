set(
    seismic_acquisition_sources
    "seismic_acquisition.c"
    "seismic_source_registry.c"
    "seismic_source_common.c"
)

set(seismic_acquisition_backend_requires)

if(CONFIG_SEISMIC_SOURCE_TEST)
    list(APPEND
        seismic_acquisition_sources
        "seismic_source_test.c"
    )

    list(APPEND
        seismic_acquisition_backend_requires
        seismic_test_generator
    )
elseif(CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH)
    list(APPEND
        seismic_acquisition_sources
        "seismic_source_esp_adc.c"
    )

    list(APPEND
        seismic_acquisition_backend_requires
        geophone_adc_test
    )
elseif(CONFIG_SEISMIC_SOURCE_AD7606)
    list(APPEND
        seismic_acquisition_sources
        "seismic_source_ad7606.c"
    )

    list(APPEND
        seismic_acquisition_backend_requires
        ad7606_acquisition
        ad7606_driver
    )
else()
    message(FATAL_ERROR "No seismic acquisition source selected")
endif()

idf_component_register(
    SRCS
        ${seismic_acquisition_sources}

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        device_status
        seismic_buffer
        log
        ${seismic_acquisition_backend_requires}
)
