idf_component_register(
    SRCS
        main.c

    INCLUDE_DIRS
        "."

    REQUIRES
        spi_bus
        display
        sd_card
        storage_manager
        storage_service
        object_recovery
        object_store
        object_service
        power_control
        keypad
        device_status
        seismic_buffer
        seismic_test_generator
        wifi_manager
        web_server
        captive_portal
        ui
        freertos
        log
)