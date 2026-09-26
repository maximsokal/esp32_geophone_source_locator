#include "ui_internal.h"

#include <string.h>


void ui_render_boot_error_locked(const char *message)
{
    const char *display_message = message;
    if (message != NULL && strcmp(message, "MOUNT FAILED") == 0) {
        display_message = ui_tr(UI_I18N_KEY_BOOT_MOUNT_FAILED);
    } else if (message != NULL && strcmp(message, "WRITE TEST FAILED") == 0) {
        display_message = ui_tr(UI_I18N_KEY_BOOT_WRITE_TEST_FAILED);
    }

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_BOOT_ERROR_TITLE),
        ui_tr(UI_I18N_KEY_BOOT_ERROR_FOOTER)
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
        ui_tr(UI_I18N_KEY_BOOT_STORAGE_FAILURE),
        UI_COLOR_ERROR
    );

    (void)ui_create_label_locked(
        card,
        14,
        46,
        260,
        display_message,
        UI_COLOR_TEXT
    );

    (void)ui_create_label_locked(
        card,
        14,
        76,
        260,
        ui_tr(UI_I18N_KEY_BOOT_SD_LIMIT),
        UI_COLOR_WARNING
    );

    (void)ui_create_label_locked(
        card,
        14,
        98,
        260,
        ui_tr(UI_I18N_KEY_BOOT_CHECK_WIRING),
        UI_COLOR_TEXT
    );
}
