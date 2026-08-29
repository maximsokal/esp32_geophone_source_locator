idf_component_register(
    SRCS
        "display.c"
        "display_driver.c"
        "display_panel.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        board_config
        spi_bus
        esp_lcd
        esp_lcd_ili9341
        log
)
