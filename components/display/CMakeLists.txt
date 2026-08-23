idf_component_register(
    SRCS
        "display.c"
        "font5x7.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board_config
        spi_bus
        esp_lcd
        esp_lcd_ili9341
        freertos
        log
)