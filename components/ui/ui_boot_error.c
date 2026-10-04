#include "ui_internal.h"

#include <stdio.h>
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


void ui_render_boot_recovery_locked(
    const char *reason,
    const char *technical_code,
    const char *warning,
    const char *primary_hint,
    const char *retry_hint,
    const char *cancel_hint
)
{
    char technical_head[40];
    const char *technical_tail = NULL;
    const size_t technical_length = technical_code != NULL
        ? strlen(technical_code)
        : 0U;
    const char *split = technical_length > 34U
        ? strchr(technical_code, ' ')
        : NULL;
    int warning_y = 53;

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_BOOT_RECOVERY_TITLE),
        ui_tr(UI_I18N_KEY_BOOT_FOOTER)
    );
    if (screen == NULL) {
        return;
    }

    lv_obj_t *card = ui_create_card_locked(screen, 12, 38, 296, 172);
    if (card == NULL) {
        return;
    }

    (void)ui_create_label_locked(card, 14, 9, 268, reason, UI_COLOR_TEXT);
    if (split != NULL) {
        const size_t head_length = (size_t)(split - technical_code);
        const size_t copy_length = head_length < sizeof(technical_head) - 1U
            ? head_length
            : sizeof(technical_head) - 1U;
        memcpy(technical_head, technical_code, copy_length);
        technical_head[copy_length] = '\0';
        technical_tail = split + 1;
        (void)ui_create_label_locked(card, 14, 31, 268, technical_head, UI_COLOR_MUTED);
        (void)ui_create_label_locked(card, 14, 49, 268, technical_tail, UI_COLOR_MUTED);
        warning_y = 69;
    } else {
        (void)ui_create_label_locked(card, 14, 31, 268, technical_code, UI_COLOR_MUTED);
    }

    if (warning != NULL && warning[0] != '\0') {
        (void)ui_create_label_locked(card, 14, warning_y, 268, warning, UI_COLOR_WARNING);
        (void)ui_create_label_locked(card, 14, warning_y + 22, 268, primary_hint, UI_COLOR_TEXT);
        (void)ui_create_label_locked(card, 14, warning_y + 44, 268, retry_hint, UI_COLOR_TEXT);
        (void)ui_create_label_locked(card, 14, warning_y + 66, 268, cancel_hint, UI_COLOR_MUTED);
    } else {
        (void)ui_create_label_locked(card, 14, warning_y, 268, primary_hint, UI_COLOR_WARNING);
        (void)ui_create_label_locked(card, 14, warning_y + 22, 268, retry_hint, UI_COLOR_TEXT);
        (void)ui_create_label_locked(card, 14, warning_y + 44, 268, cancel_hint, UI_COLOR_MUTED);
    }
}


void ui_render_boot_recovery_result_locked(
    bool success,
    bool formatted,
    const char *error_name
)
{
    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_BOOT_RECOVERY_TITLE),
        ui_tr(UI_I18N_KEY_BOOT_FOOTER)
    );
    if (screen == NULL) {
        return;
    }

    lv_obj_t *card = ui_create_card_locked(screen, 14, 64, 292, 106);
    if (card == NULL) {
        return;
    }

    const ui_i18n_key_t status_key = success
        ? (formatted
            ? UI_I18N_KEY_BOOT_FORMAT_COMPLETE
            : UI_I18N_KEY_BOOT_RECOVERY_CARD_READY)
        : UI_I18N_KEY_BOOT_FORMAT_FAILED;
    (void)ui_create_label_locked(
        card,
        14,
        20,
        264,
        ui_tr(status_key),
        success ? UI_COLOR_GOOD : UI_COLOR_ERROR
    );

    char error_line[48];
    (void)snprintf(
        error_line,
        sizeof(error_line),
        "err=%s",
        error_name != NULL ? error_name : "ESP_ERR_UNKNOWN"
    );
    (void)ui_create_label_locked(
        card,
        14,
        51,
        264,
        error_line,
        UI_COLOR_MUTED
    );
}
