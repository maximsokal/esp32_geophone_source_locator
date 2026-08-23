#pragma once

#include <stdint.h>

/*
 * Returns a 5-column, 7-row bitmap glyph.
 *
 * Each byte represents one vertical column.
 * Bit 0 is the top pixel.
 */
const uint8_t *font5x7_get_glyph(char character);