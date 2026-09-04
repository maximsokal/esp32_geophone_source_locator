idf_component_register(
    SRCS
        "ad7606_driver.c"
        "ad7606_presence.c"
        "ad7606_presence_policy.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        board_config
        esp_driver_gpio
        esp_driver_spi
        esp_timer
        esp_rom
        freertos
        log
)
