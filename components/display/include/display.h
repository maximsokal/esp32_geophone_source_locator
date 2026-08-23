#pragma once

#include <stdint.h>

#include "esp_err.h"


#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240


/*
 * RGB565 colors.
 */
#define DISPLAY_COLOR_BLACK       0x0000
#define DISPLAY_COLOR_WHITE       0xFFFF
#define DISPLAY_COLOR_RED         0xF800
#define DISPLAY_COLOR_GREEN       0x07E0
#define DISPLAY_COLOR_BLUE        0x001F
#define DISPLAY_COLOR_YELLOW      0xFFE0
#define DISPLAY_COLOR_CYAN        0x07FF
#define DISPLAY_COLOR_GRAY        0x8410
#define DISPLAY_COLOR_DARK_GRAY   0x4208
#define DISPLAY_COLOR_NAVY        0x000F


esp_err_t display_init(void);


esp_err_t display_fill(
    uint16_t color
);


esp_err_t display_fill_rect(
    int x,
    int y,
    int width,
    int height,
    uint16_t color
);


/*
 * Draw an RGB565 bitmap.
 *
 * The source buffer does not need to be DMA-capable.
 * The display component copies it into its internal DMA buffer
 * and transfers the image in chunks.
 */
esp_err_t display_draw_rgb565_bitmap(
    int x,
    int y,
    int width,
    int height,
    const uint16_t *pixels
);


esp_err_t display_draw_char(
    int x,
    int y,
    char character,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
);


esp_err_t display_draw_text(
    int x,
    int y,
    const char *text,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
);


void display_deinit(void);