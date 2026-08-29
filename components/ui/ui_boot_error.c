#include "ui_internal.h"


#define UI_BOOT_ERROR_COLOR_ERROR    0xF05A67
#define UI_BOOT_ERROR_COLOR_TEXT     0xF3F7FB
#define UI_BOOT_ERROR_COLOR_WARNING  0xF3B84B


static lv_obj_t *ui_boot_error_label(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
)
{
    lv_obj_t *label =
        lv_label_create(
            parent
        );

    lv_obj_set_pos(
        label,
        x,
        y
    );

    lv_obj_set_width(
        label,
        width
    );

    lv_label_set_text(
        label,
        text
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


void ui_render_boot_error_locked(
    const char *message
)
{
    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "BOOT ERROR",
            "PRESS POWER TO SHUT DOWN"
        );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            16,
            55,
            288,
            126
        );

    ui_boot_error_label(
        card,
        14,
        14,
        260,
        "SD CARD / STORAGE FAILURE",
        UI_BOOT_ERROR_COLOR_ERROR
    );

    ui_boot_error_label(
        card,
        14,
        46,
        260,
        message,
        UI_BOOT_ERROR_COLOR_TEXT
    );

    ui_boot_error_label(
        card,
        14,
        76,
        260,
        "SD SPI FIXED AT 2 MHz",
        UI_BOOT_ERROR_COLOR_WARNING
    );

    ui_boot_error_label(
        card,
        14,
        98,
        260,
        "CHECK CARD, CS AND WIRING",
        UI_BOOT_ERROR_COLOR_TEXT
    );
}
