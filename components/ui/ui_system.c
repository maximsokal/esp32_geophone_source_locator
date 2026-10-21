#include "ui_internal.h"
#include "ui_system_model.h"
#include "ui_utf8_format.h"

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

    if (context->system_hardware_open) {
        ui_render_hardware_locked();
        return;
    }

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

    ui_utf8_format(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_FIRMWARE),
        app != NULL ? app->version : ui_tr(UI_I18N_KEY_SHARED_EMPTY)
    );
    (void)ui_create_label_locked(
        card, 10, 8, 284, text, UI_COLOR_ACCENT
    );

    ui_utf8_format(
        text,
        sizeof(text),
        ui_tr(UI_I18N_KEY_SYSTEM_IDF_VERSION),
        esp_get_idf_version()
    );
    (void)ui_create_label_locked(
        card, 10, 27, 284, text, UI_COLOR_TEXT
    );

    ui_utf8_format(
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

    const ui_system_entry_t selected_entry =
        context->system_hardware_selected
            ? UI_SYSTEM_ENTRY_HARDWARE
            : ui_system_model_entry_from_locale(
                context->system_language_selection
            );
    const ui_i18n_key_t selected_key =
        selected_entry == UI_SYSTEM_ENTRY_HARDWARE
            ? UI_I18N_KEY_TFT_HW_TITLE
            : selected_entry == UI_SYSTEM_ENTRY_UKRAINIAN
                ? UI_I18N_KEY_SYSTEM_LANGUAGE_UKRAINIAN
                : UI_I18N_KEY_SYSTEM_LANGUAGE_ENGLISH;

    (void)ui_create_row_locked(
        card,
        142,
        ui_tr(selected_key),
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

    ui_utf8_format(
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

    ui_utf8_format(
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

    ui_utf8_format(
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


static ui_system_key_t ui_system_model_key_from_keypad(keypad_key_t key)
{
    switch (key) {
        case KEYPAD_KEY_UP:
            return UI_SYSTEM_KEY_UP;
        case KEYPAD_KEY_DOWN:
            return UI_SYSTEM_KEY_DOWN;
        case KEYPAD_KEY_LEFT:
            return UI_SYSTEM_KEY_LEFT;
        case KEYPAD_KEY_RIGHT:
            return UI_SYSTEM_KEY_RIGHT;
        case KEYPAD_KEY_OK:
            return UI_SYSTEM_KEY_OK;
        default:
            return UI_SYSTEM_KEY_OTHER;
    }
}


void ui_system_handle_key_locked(keypad_key_t key)
{
    ui_context_t *context = ui_context();
    ui_system_state_t state = {
        .selected = context->system_hardware_selected
            ? UI_SYSTEM_ENTRY_HARDWARE
            : ui_system_model_entry_from_locale(
                context->system_language_selection
            ),
        .hardware_open = context->system_hardware_open,
    };
    ui_system_state_t next;
    const ui_system_action_t action = ui_system_model_on_key(
        &state,
        ui_system_model_key_from_keypad(key),
        context->system_language_selection == ui_i18n_get_locale(),
        &next
    );

    switch (action) {
        case UI_SYSTEM_ACTION_SELECT_EN:
        case UI_SYSTEM_ACTION_SELECT_UK: {
            ui_locale_t selection;
            if (!ui_system_model_entry_locale(next.selected, &selection)) {
                return;
            }
            context->system_language_selection = selection;
            context->system_hardware_selected = false;
            ui_render_system_locked();
            return;
        }
        case UI_SYSTEM_ACTION_SELECT_HARDWARE:
            context->system_hardware_selected = true;
            ui_render_system_locked();
            return;
        case UI_SYSTEM_ACTION_APPLY_LANGUAGE: {
            const ui_locale_t locale = context->system_language_selection;
            ui_i18n_set_locale(locale);
            if (ui_event_dispatcher_queue_locale_save(locale) != ESP_OK) {
                ui_i18n_set_locale(UI_LOCALE_EN);
                context->system_language_selection = UI_LOCALE_EN;
            }
            ui_render_system_locked();
            return;
        }
        case UI_SYSTEM_ACTION_OPEN_HARDWARE:
        case UI_SYSTEM_ACTION_CLOSE_HARDWARE:
            context->system_hardware_open = next.hardware_open;
            ui_render_system_locked();
            return;
        case UI_SYSTEM_ACTION_GO_HOME:
            context->system_hardware_selected = false;
            ui_screen_open_locked(UI_SCREEN_HOME);
            return;
        case UI_SYSTEM_ACTION_NONE:
        default:
            return;
    }
}
