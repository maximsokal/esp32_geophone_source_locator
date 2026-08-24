#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"


#define DISPLAY_TRANSFER_ROWS        20

#define DISPLAY_FONT_GLYPH_WIDTH      5
#define DISPLAY_FONT_GLYPH_HEIGHT     7
#define DISPLAY_FONT_CELL_WIDTH       6
#define DISPLAY_FONT_CELL_HEIGHT      8
#define DISPLAY_FONT_MAX_SCALE        4

#define DISPLAY_TEXT_SCRATCH_ROWS \
    (DISPLAY_FONT_CELL_HEIGHT * DISPLAY_FONT_MAX_SCALE)


esp_err_t display_transfer_init(void);


void display_transfer_deinit(void);


esp_err_t display_transfer_lock(void);


void display_transfer_unlock(void);


void display_transfer_set_panel(
    esp_lcd_panel_handle_t panel_handle
);


esp_lcd_panel_handle_t display_transfer_panel(void);


bool display_transfer_has_panel(void);


esp_err_t display_buffers_init(void);


void display_buffers_deinit(void);


bool display_buffers_is_ready(void);


uint16_t *display_buffers_dma(void);


size_t display_buffers_dma_capacity_pixels(void);


uint16_t *display_buffers_text(void);


size_t display_buffers_text_capacity_pixels(void);


esp_err_t display_dma_init(void);


void display_dma_deinit(void);


bool display_dma_is_ready(void);


esp_err_t display_dma_register_io(
    esp_lcd_panel_io_handle_t io_handle
);


/*
 * Caller must hold the display recursive mutex.
 */
esp_err_t display_dma_draw_bitmap_locked(
    int x_start,
    int y_start,
    int x_end,
    int y_end,
    const uint16_t *pixels
);


esp_err_t display_driver_init(void);


void display_driver_deinit(void);


bool display_driver_is_initialized(void);


esp_err_t display_panel_configure(
    esp_lcd_panel_handle_t panel_handle
);


esp_err_t display_bitmap_draw_locked(
    int x,
    int y,
    int width,
    int height,
    const uint16_t *pixels
);


void display_font_render_character(
    uint16_t *pixels,
    int stride,
    int x_offset,
    char character,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
);


esp_err_t display_char_draw_locked(
    int x,
    int y,
    char character,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
);


static inline bool display_internal_is_ready(void)
{
    return
        display_transfer_has_panel() &&
        display_buffers_is_ready() &&
        display_dma_is_ready();
}
