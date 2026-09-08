#include "ui_internal.h"


void ui_render_boot_error_locked(const char *message)
{
    lv_obj_t *screen = ui_create_screen_base_locked(
        "BOOT ERROR",
        "PRESS POWER TO SHUT DOWN"
    );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *card = ui_create_card_locked(
        screen, 16, 55, 288, 126
    );

    if (card == NULL) {
        return;
    }

    (void)ui_create_label_locked(
        card,
        14,
        14,
        260,
        "SD CARD / STORAGE FAILURE",
        UI_COLOR_ERROR
    );

    (void)ui_create_label_locked(
        card,
        14,
        46,
        260,
        message,
        UI_COLOR_TEXT
    );

    (void)ui_create_label_locked(
        card,
        14,
        76,
        260,
        "SD SPI CEILING 20 MHz",
        UI_COLOR_WARNING
    );

    (void)ui_create_label_locked(
        card,
        14,
        98,
        260,
        "CHECK CARD, CS AND WIRING",
        UI_COLOR_TEXT
    );
}
