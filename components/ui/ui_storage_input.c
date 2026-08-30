#include "ui_internal.h"

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
                "REFRESHING SD INFO..."
            );

            esp_err_t err =
                storage_service_info_async();

            if (err != ESP_OK) {
                ui_set_notice_locked(
                    "STORAGE SERVICE BUSY/FAILED"
                );
            }
        } else {
            if (!storage_manager_is_ready()) {
                context->storage_erase_confirm =
                    false;
                return;
            }

            if (!context->storage_erase_confirm) {
                context->storage_erase_confirm =
                    true;

                ui_set_notice_locked(
                    "PRESS OK AGAIN TO ERASE DATA"
                );
            } else {
                context->storage_erase_confirm =
                    false;

                ui_set_notice_locked(
                    "ERASING DATA..."
                );

                esp_err_t err =
                    storage_service_erase_data_async();

                if (err != ESP_OK) {
                    ui_set_notice_locked(
                        "STORAGE SERVICE BUSY/FAILED"
                    );
                }
            }
        }

        ui_render_storage_locked();
    }
}
