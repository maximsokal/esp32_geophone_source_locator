#include "ui_internal.h"


static bool ui_objects_measurement_mutation_allowed_locked(void)
{
    measurement_service_snapshot_t measurement;
    if (measurement_service_get_snapshot(&measurement) != ESP_OK) {
        ui_set_notice_locked("MEASUREMENT SERVICE UNAVAILABLE");
        ui_render_objects_locked();
        return false;
    }

    if (!measurement_service_snapshot_allows_object_mutation(&measurement)) {
        ui_set_notice_locked("MEASUREMENT OPERATION IN PROGRESS");
        ui_render_objects_locked();
        return false;
    }

    return true;
}


void ui_handle_objects_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (object_service_is_busy()) {
        return;
    }

    if (context->objects_mode == UI_OBJECTS_MENU) {
        if (key == KEYPAD_KEY_UP) {
            ui_move_index(
                &context->object_action,
                3,
                false
            );

            ui_render_objects_locked();
        } else if (key == KEYPAD_KEY_DOWN) {
            ui_move_index(
                &context->object_action,
                3,
                true
            );

            ui_render_objects_locked();
        } else if (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        ) {
            if (context->object_action == 0) {
                if (!ui_objects_measurement_mutation_allowed_locked()) {
                    return;
                }

                ui_set_notice_locked(
                    "CREATING OBJECT..."
                );

                esp_err_t err =
                    object_service_create_async();

                if (err != ESP_OK) {
                    ui_set_notice_locked(
                        "OBJECT SERVICE BUSY/FAILED"
                    );
                }

                ui_render_objects_locked();
                return;
            }

            if (
                !context->objects_loaded ||
                context->object_count == 0
            ) {
                return;
            }

            context->objects_mode =
                context->object_action == 1
                    ? UI_OBJECTS_SELECT_ACTIVE
                    : UI_OBJECTS_SELECT_DELETE;

            context->object_index = 0;
            ui_set_notice_locked(NULL);
            ui_render_objects_locked();
        } else if (key == KEYPAD_KEY_LEFT) {
            ui_open_home_locked();
        }

        return;
    }

    if (context->objects_mode == UI_OBJECTS_DELETE_CONFIRM) {
        if (key == KEYPAD_KEY_LEFT) {
            context->objects_mode =
                UI_OBJECTS_SELECT_DELETE;

            ui_render_objects_locked();
        } else if (
            key == KEYPAD_KEY_OK ||
            key == KEYPAD_KEY_RIGHT
        ) {
            if (
                context->object_count > 0 &&
                context->object_index <
                    context->object_count
            ) {
                if (!ui_objects_measurement_mutation_allowed_locked()) {
                    return;
                }

                esp_err_t err =
                    object_service_delete_async(
                        context->objects[
                            context->object_index
                        ].id
                    );

                ui_set_notice_locked(
                    err == ESP_OK
                        ? "DELETING..."
                        : "DELETE FAILED TO START"
                );

                ui_render_objects_locked();
            }
        }

        return;
    }

    if (key == KEYPAD_KEY_LEFT) {
        context->objects_mode =
            UI_OBJECTS_MENU;

        context->object_action =
            0;

        ui_set_notice_locked(
            NULL
        );

        ui_render_objects_locked();

        return;
    }

    if (context->object_count == 0) {
        return;
    }

    if (key == KEYPAD_KEY_UP) {
        ui_move_index(
            &context->object_index,
            context->object_count,
            false
        );

        ui_render_objects_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(
            &context->object_index,
            context->object_count,
            true
        );

        ui_render_objects_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        if (
            context->object_index >=
            context->object_count
        ) {
            return;
        }

        if (
            context->objects_mode ==
            UI_OBJECTS_SELECT_ACTIVE
        ) {
            if (!ui_objects_measurement_mutation_allowed_locked()) {
                return;
            }

            esp_err_t err =
                object_service_set_active_async(
                    context->objects[
                        context->object_index
                    ].id
                );

            ui_set_notice_locked(
                err == ESP_OK
                    ? "SETTING ACTIVE..."
                    : "SET ACTIVE FAILED TO START"
            );

            ui_render_objects_locked();
        } else {
            context->objects_mode =
                UI_OBJECTS_DELETE_CONFIRM;

            ui_set_notice_locked(
                NULL
            );

            ui_render_objects_locked();
        }
    }
}
