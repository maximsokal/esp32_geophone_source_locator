set(
    seismic_acquisition_sources
    "seismic_acquisition.c"
    "seismic_source_registry.c"
    "seismic_source_common.c"
)

if(CONFIG_SEISMIC_SOURCE_TEST)
    list(APPEND
        seismic_acquisition_sources
        "seismic_source_test.c"
    )
elseif(CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH)
    list(APPEND
        seismic_acquisition_sources
        "seismic_source_esp_adc.c"
    )
elseif(CONFIG_SEISMIC_SOURCE_AD7606)
    list(APPEND
        seismic_acquisition_sources
        "seismic_source_ad7606.c"
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

    # ESP-IDF 5.5 uses the v1 build system: REQUIRES/PRIV_REQUIRES
    # cannot depend on CONFIG_* because dependency expansion happens
    # before sdkconfig is loaded. Keep backend components private and
    # unconditional here, while compiling only the selected adapter
    # above. Unselected backend archives have no referenced symbols and
    # are therefore discarded by the final linker's section GC.
    PRIV_REQUIRES
        ad7606_acquisition
        ad7606_driver
        app_config
        device_status
        geophone_adc_test
        seismic_buffer
        seismic_test_generator
        log
)
