idf_component_register(
    SRCS
        "display.c"
        "display_driver.c"
        "display_panel.c"
        "display_transfer.c"
        "display_dma.c"
        "display_buffers.c"
        "display_fill.c"
        "display_bitmap.c"
        "display_font_render.c"
        "display_char.c"
        "display_text.c"
        "display_text_fast.c"
        "font5x7.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        board_config
        spi_bus
        esp_lcd
        esp_lcd_ili9341
        freertos
        log
)
