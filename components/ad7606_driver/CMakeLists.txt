idf_component_register(
    SRCS
        "ad7606_driver.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board_config
        spi_bus
        esp_driver_gpio
        esp_driver_spi
        esp_timer
        esp_rom
        log
)