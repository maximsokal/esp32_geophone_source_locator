#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"


static void ui_system_label_set_if_changed(
    lv_obj_t *label,
    const char *text
)
{
    if (label == NULL || text == NULL) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}


void ui_render_system_locked(void)
{
    ui_context_t *context = ui_context();

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_SYSTEM_TITLE),
        ui_tr(UI_I18N_KEY_SYSTEM_LANGUAGE_FOOTER)
    );

    if (screen == NULL) {
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *card = ui_create_card_locked(screen, 10, 38, 300, 176);

    if (card == NULL) {
        return;
    }

    char text[96];

    snprintf(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_FIRMWARE),
        app != NULL ? app->version : ui_tr(UI_I18N_KEY_SHARED_EMPTY)
    );
    (void)ui_create_label_locked(
        card, 10, 8, 284, text, UI_COLOR_ACCENT
    );

    snprintf(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_IDF_VERSION),
        esp_get_idf_version()
    );
    (void)ui_create_label_locked(
        card, 10, 27, 284, text, UI_COLOR_TEXT
    );

    snprintf(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_LVGL_VERSION),
        LVGL_VERSION_MAJOR,
        LVGL_VERSION_MINOR,
        LVGL_VERSION_PATCH
    );
    (void)ui_create_label_locked(
        card, 10, 46, 284, text, UI_COLOR_GOOD
    );

    context->system_heap = ui_create_label_locked(
        card,
        10,
        65,
        284,
        ui_tr(UI_I18N_KEY_SYSTEM_HEAP_EMPTY),
        UI_COLOR_TEXT
    );

    context->system_psram = ui_create_label_locked(
        card,
        10,
        84,
        284,
        ui_tr(UI_I18N_KEY_SYSTEM_PSRAM_EMPTY),
        UI_COLOR_TEXT
    );

    context->system_uptime = ui_create_label_locked(
        card,
        10,
        103,
        284,
        ui_tr(UI_I18N_KEY_SYSTEM_UPTIME_EMPTY),
        UI_COLOR_MUTED
    );

    (void)ui_create_label_locked(
        card,
        10,
        122,
        284,
        ui_tr(UI_I18N_KEY_SYSTEM_CORES),
        UI_COLOR_MUTED
    );

    (void)ui_create_row_locked(
        card,
        140,
        ui_tr(context->system_language_selection == UI_LOCALE_UK
            ? UI_I18N_KEY_SYSTEM_LANGUAGE_UKRAINIAN
            : UI_I18N_KEY_SYSTEM_LANGUAGE_ENGLISH),
        true,
        true
    );

    ui_system_refresh_locked();
}


void ui_system_refresh_locked(void)
{
    ui_context_t *context = ui_context();

    if (
        context->system_heap == NULL ||
        context->system_psram == NULL ||
        context->system_uptime == NULL
    ) {
        return;
    }

    char text[96];

    snprintf(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_HEAP),
        (unsigned long)(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U
        )
    );
    ui_system_label_set_if_changed(
        context->system_heap,
        text
    );

    snprintf(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_PSRAM),
        (unsigned long)(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U
        )
    );
    ui_system_label_set_if_changed(
        context->system_psram,
        text
    );

    snprintf(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_UPTIME),
        (long long)(esp_timer_get_time() / 1000000LL)
    );
    ui_system_label_set_if_changed(
        context->system_uptime,
        text
    );
}


void ui_system_handle_key_locked(keypad_key_t key)
{
    ui_context_t *context = ui_context();
    if (key == KEYPAD_KEY_LEFT) {
        ui_screen_open_locked(UI_SCREEN_HOME);
        return;
    }

    if (key == KEYPAD_KEY_UP || key == KEYPAD_KEY_DOWN) {
        const ui_locale_t selection =
            key == KEYPAD_KEY_UP ? UI_LOCALE_EN : UI_LOCALE_UK;
        if (context->system_language_selection == selection) {
            return;
        }
        context->system_language_selection = selection;
        ui_render_system_locked();
        return;
    }

    if (key != KEYPAD_KEY_OK && key != KEYPAD_KEY_RIGHT) {
        return;
    }

    const ui_locale_t locale = context->system_language_selection;
    if (locale == ui_i18n_get_locale()) {
        return;
    }

    ui_i18n_set_locale(locale);
    if (ui_event_dispatcher_queue_locale_save(locale) != ESP_OK) {
        ui_i18n_set_locale(UI_LOCALE_EN);
        context->system_language_selection = UI_LOCALE_EN;
    }
    ui_render_system_locked();
}
