idf_component_register(
    SRCS
        main.c

    INCLUDE_DIRS
        "."

    REQUIRES
        spi_bus
        display
        sd_card
        power_control
        keypad
        device_status
        wifi_manager
        web_server
        ui
        freertos
        log
)