idf_component_register(
    SRCS
        "seismic_acquisition.c"
        "seismic_rate_profile.c"
        "seismic_source_registry.c"
        "seismic_source_common.c"
        "seismic_source_test.c"
        "seismic_source_ad7606.c"
        "seismic_source_ad7606_inventory_policy.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    # ESP-IDF 5.5 build-system v1 resolves the component dependency
    # graph before sdkconfig is available, so CONFIG_* must not control
    # SRCS or REQUIRES here. All backend adapter translation units are
    # compiled, but the registry references only the selected descriptor.
    # Because each adapter is a separate object file, unselected adapters
    # and their backend archives remain unreferenced and can be omitted
    # from the final ELF by normal static-library resolution / GC.
    REQUIRES
        hw_inventory

    PRIV_REQUIRES
        ad7606_acquisition
        ad7606_driver
        app_config
        device_status
        freertos
        esp_timer
        seismic_buffer
        seismic_test_generator
        log
)
