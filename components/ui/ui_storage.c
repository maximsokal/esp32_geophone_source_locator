#include "ui_internal.h"

#include <stdio.h>

#include "esp_lvgl_port.h"

#include "storage_manager.h"
#include "ui_fonts.h"
#include "ui_utf8_format.h"


#define UI_STORAGE_COLOR_TEXT     0xF3F7FB
#define UI_STORAGE_COLOR_MUTED    0x94A7BC
#define UI_STORAGE_COLOR_ACCENT   0x21C7D9
#define UI_STORAGE_COLOR_WARNING  0xF3B84B


static lv_obj_t *ui_storage_label(
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
    if (label == NULL) return NULL;

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
    lv_obj_set_style_text_font(label, &ui_font_montserrat_14, 0);

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


static void ui_storage_render_notice(
    lv_obj_t *screen
)
{
    if (ui_context()->notice[0] == '\0') {
        return;
    }

    ui_storage_label(
        screen,
        12,
        199,
        296,
        ui_context()->notice,
        UI_STORAGE_COLOR_WARNING
    );
}


void ui_render_storage_locked(void)
{
    ui_context_t *context =
        ui_context();

    const bool storage_ready =
        storage_manager_is_ready();

    if (!storage_ready) {
        context->storage_erase_confirm =
            false;
    }

    lv_obj_t *screen =
        ui_create_screen_base_locked(
            ui_tr(UI_I18N_KEY_STORAGE_TITLE),
            ui_tr(UI_I18N_KEY_SHARED_FOOTER_BACK)
        );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            10,
            42,
            300,
            82
        );

    char text[96];

    if (context->storage_info_valid) {
        const double total_gb =
            (double)context->storage_info.total_bytes /
            (1024.0 * 1024.0 * 1024.0);

        const double free_gb =
            (double)context->storage_info.free_bytes /
            (1024.0 * 1024.0 * 1024.0);

        ui_utf8_format(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_STORAGE_SUMMARY),
            context->storage_info.name,
            context->storage_info.type,
            total_gb
        );

        ui_storage_label(
            card,
            10,
            10,
            280,
            text,
            UI_STORAGE_COLOR_ACCENT
        );

        ui_utf8_format(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_STORAGE_SPACE),
            free_gb,
            (double)context->storage_info.speed_khz /
                1000.0
        );

        ui_storage_label(
            card,
            10,
            33,
            280,
            text,
            UI_STORAGE_COLOR_TEXT
        );

        ui_storage_label(
            card,
            10,
            56,
            280,
            ui_tr(UI_I18N_KEY_STORAGE_RUNTIME_LIMIT),
            UI_STORAGE_COLOR_MUTED
        );
    } else {
        ui_storage_label(
            card,
            10,
            24,
            280,
            ui_tr(UI_I18N_KEY_STORAGE_NOT_LOADED),
            UI_STORAGE_COLOR_MUTED
        );
    }

    ui_create_row_locked(
        screen,
        132,
        ui_tr(UI_I18N_KEY_STORAGE_REFRESH),
        context->storage_action == 0,
        true
    );

    ui_create_row_locked(
        screen,
        166,
        context->storage_erase_confirm
            ? ui_tr(UI_I18N_KEY_STORAGE_CONFIRM_ERASE)
            : ui_tr(UI_I18N_KEY_STORAGE_ERASE_DEVICE),
        context->storage_action == 1,
        storage_ready
    );

    ui_storage_render_notice(
        screen
    );
}


void ui_storage_service_event(
    const storage_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    ui_context_t *context =
        ui_context();

    if (
        event == NULL ||
        !atomic_load_explicit(
            &context->runtime_ready,
            memory_order_acquire
        )
    ) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    switch (event->type) {
        case STORAGE_SERVICE_EVENT_INFO_DONE:
            context->storage_info =
                event->info;

            context->storage_info_valid =
                true;

            ui_set_notice_locked(
                NULL
            );
            break;

        case STORAGE_SERVICE_EVENT_ERASE_DONE:
            /* ERASE_DONE does not carry refreshed card statistics. */
            context->storage_info_valid =
                false;

            context->storage_erase_confirm =
                false;

            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_STORAGE_ERASE_COMPLETE)
            );
            break;

        case STORAGE_SERVICE_EVENT_INFO_FAILED:
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_STORAGE_INFO_FAILED)
            );
            break;

        case STORAGE_SERVICE_EVENT_ERASE_FAILED:
        default:
            context->storage_erase_confirm =
                false;

            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_STORAGE_ERASE_FAILED)
            );
            break;
    }

    if (context->screen == UI_SCREEN_STORAGE) {
        ui_render_storage_locked();
    }

    lvgl_port_unlock();
}
