#pragma once

/*
 * Stage 11D central LVGL theme.
 *
 * Screen-specific renderers must use these shared semantic colors
 * instead of defining private palettes. This keeps later UI changes
 * local to one file instead of spreading color constants across the
 * feature screens.
 */
#define UI_COLOR_BACKGROUND  0x08111F
#define UI_COLOR_HEADER      0x0E1A2B
#define UI_COLOR_PANEL       0x132238
#define UI_COLOR_PANEL_ALT   0x1B2D46

#define UI_COLOR_ACCENT      0x21C7D9
#define UI_COLOR_GOOD        0x4CD681
#define UI_COLOR_WARNING     0xF3B84B
#define UI_COLOR_ERROR       0xF05A67
#define UI_COLOR_LINK_MEASURING 0x4A90E2

#define UI_COLOR_TEXT        0xF3F7FB
#define UI_COLOR_MUTED       0x94A7BC

#define UI_MENU_ROW_HEIGHT   31
#define UI_MENU_ROW_STEP     34
#define UI_MENU_LABEL_Y      8
