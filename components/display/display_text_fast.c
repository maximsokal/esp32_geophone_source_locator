#include "display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "font5x7.h"


#define FONT_GLYPH_WIDTH    5
#define FONT_GLYPH_HEIGHT   7
#define FONT_CELL_WIDTH     6
#define FONT_CELL_HEIGHT    8
#define FONT_MAX_SCALE      4


static void display_text_fast_render_run(
    uint16_t *pixels,
    int width,
    const char *text,
    size_t text_length,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
)
{
    const int cell_width =
        FONT_CELL_WIDTH * scale;

    const int cell_height =
        FONT_CELL_HEIGHT * scale;


    for (
        size_t character_index = 0;
        character_index < text_length;
        ++character_index
    ) {
        const uint8_t *glyph =
            font5x7_get_glyph(
                text[character_index]
            );


        const int character_x =
            (int)character_index *
            cell_width;


        for (
            int py = 0;
            py < cell_height;
            ++py
        ) {
            const int source_y =
                py / scale;


            for (
                int px = 0;
                px < cell_width;
                ++px
            ) {
                const int source_x =
                    px / scale;


                bool pixel_on =
                    false;


                if (
                    source_x <
                        FONT_GLYPH_WIDTH &&
                    source_y <
                        FONT_GLYPH_HEIGHT
                ) {
                    pixel_on =
                        (
                            glyph[source_x] &
                            (1U << source_y)
                        ) != 0;
                }


                const int destination_x =
                    character_x +
                    px;


                pixels[
                    py * width +
                    destination_x
                ] =
                    pixel_on
                        ? foreground
                        : background;
            }
        }
    }
}


esp_err_t display_draw_text_fast(
    int x,
    int y,
    const char *text,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        scale == 0 ||
        scale > FONT_MAX_SCALE
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        x < 0 ||
        y < 0 ||
        x >= DISPLAY_WIDTH ||
        y >= DISPLAY_HEIGHT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    const int cell_width =
        FONT_CELL_WIDTH * scale;

    const int cell_height =
        FONT_CELL_HEIGHT * scale;


    const int available_width =
        DISPLAY_WIDTH - x;


    const size_t max_characters =
        (size_t)(
            available_width /
            cell_width
        );


    if (max_characters == 0) {
        return ESP_ERR_INVALID_SIZE;
    }


    const int maximum_run_width =
        (int)max_characters *
        cell_width;


    const size_t maximum_pixel_count =
        (size_t)maximum_run_width *
        cell_height;


    uint16_t *pixels =
        malloc(
            maximum_pixel_count *
            sizeof(uint16_t)
        );


    if (pixels == NULL) {
        return ESP_ERR_NO_MEM;
    }


    const char *cursor =
        text;


    int current_y =
        y;


    esp_err_t result =
        ESP_OK;


    while (
        *cursor != '\0' &&
        current_y +
            cell_height <=
            DISPLAY_HEIGHT
    ) {
        if (*cursor == '\n') {

            current_y +=
                cell_height;

            cursor++;

            continue;
        }


        size_t run_length =
            0;


        while (
            cursor[run_length] != '\0' &&
            cursor[run_length] != '\n' &&
            run_length <
                max_characters
        ) {
            run_length++;
        }


        if (run_length == 0) {
            break;
        }


        const int run_width =
            (int)run_length *
            cell_width;


        display_text_fast_render_run(
            pixels,
            run_width,
            cursor,
            run_length,
            foreground,
            background,
            scale
        );


        result =
            display_draw_rgb565_bitmap(
                x,
                current_y,
                run_width,
                cell_height,
                pixels
            );


        if (result != ESP_OK) {
            break;
        }


        cursor +=
            run_length;


        if (*cursor == '\n') {

            cursor++;

            current_y +=
                cell_height;

            continue;
        }


        if (*cursor != '\0') {

            current_y +=
                cell_height;
        }
    }


    free(
        pixels
    );


    return result;
}
