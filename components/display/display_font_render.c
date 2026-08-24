#include "display_internal.h"

#include "font5x7.h"


void display_font_render_character(
    uint16_t *pixels,
    int stride,
    int x_offset,
    char character,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
)
{
    const int cell_width =
        DISPLAY_FONT_CELL_WIDTH *
        scale;

    const int cell_height =
        DISPLAY_FONT_CELL_HEIGHT *
        scale;

    const uint8_t *glyph =
        font5x7_get_glyph(
            character
        );

    for (
        int py = 0;
        py < cell_height;
        ++py
    ) {
        int source_y =
            py /
            scale;

        for (
            int px = 0;
            px < cell_width;
            ++px
        ) {
            int source_x =
                px /
                scale;

            bool pixel_on =
                false;

            if (
                source_x <
                    DISPLAY_FONT_GLYPH_WIDTH &&
                source_y <
                    DISPLAY_FONT_GLYPH_HEIGHT
            ) {
                pixel_on =
                    (
                        glyph[source_x] &
                        (
                            1U <<
                            source_y
                        )
                    ) != 0;
            }

            pixels[
                py *
                    stride +
                x_offset +
                px
            ] =
                pixel_on
                    ? foreground
                    : background;
        }
    }
}
