#include "ui_internal.h"

#include "esp_lvgl_port.h"
#include "freertos/task.h"
#include "storage_manager.h"


void ui_handle_storage_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (storage_service_is_busy()) {
        return;
    }

    if (key == KEYPAD_KEY_LEFT) {
        context->storage_erase_confirm =
            false;

        ui_open_home_locked();
    } else if (key == KEYPAD_KEY_UP) {
        context->storage_erase_confirm =
            false;

        ui_move_index(
            &context->storage_action,
            2,
            false
        );

        ui_set_notice_locked(
            NULL
        );

        ui_render_storage_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        context->storage_erase_confirm =
            false;

        ui_move_index(
            &context->storage_action,
            2,
            true
        );

        ui_set_notice_locked(
            NULL
        );

        ui_render_storage_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        if (context->storage_action == 0) {
            context->storage_erase_confirm =
                false;

            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_STORAGE_REFRESHING)
            );

            esp_err_t err =
                storage_service_info_async();

            if (err != ESP_OK) {
                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_STORAGE_SERVICE_BUSY)
                );
            }
        } else {
            const size_t selected_action = context->storage_action;
            if (!ui_storage_query_begin()) {
                return;
            }

            /* Readiness takes storage_manager's mutex, so do not wait with LVGL held. */
            lvgl_port_unlock();
            const bool storage_ready = storage_manager_is_ready();
            while (!lvgl_port_lock(0)) {
                vTaskDelay(1U);
            }
            ui_storage_query_end();

            if (
                context->screen != UI_SCREEN_STORAGE ||
                context->storage_action != selected_action
            ) {
                return;
            }

            if (!storage_ready) {
                context->storage_erase_confirm =
                    false;
                return;
            }

            if (!context->storage_erase_confirm) {
                context->storage_erase_confirm =
                    true;

                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_STORAGE_CONFIRM_PROMPT)
                );
            } else {
                context->storage_erase_confirm =
                    false;

                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_STORAGE_ERASING)
                );

                esp_err_t err =
                    storage_service_erase_data_async();

                if (err != ESP_OK) {
                    ui_set_notice_locked(
                        ui_tr(UI_I18N_KEY_STORAGE_SERVICE_BUSY)
                    );
                }
            }
        }

        ui_render_storage_locked();
    }
}
