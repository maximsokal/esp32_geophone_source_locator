#include "ui_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "object_service.h"
#include "object_store.h"

#include "ui_objects_view.h"


static const char *TAG =
    "UI_OBJECTS";


typedef enum
{
    OBJECT_MODE_MENU = 0,
    OBJECT_MODE_SELECT_ACTIVE,
    OBJECT_MODE_SELECT_DELETE,
    OBJECT_MODE_DELETE_CONFIRM

} object_mode_t;


static object_mode_t s_mode =
    OBJECT_MODE_MENU;


static object_mode_t s_pending_list_mode =
    OBJECT_MODE_SELECT_ACTIVE;


static size_t s_selected_action =
    0;


static object_store_summary_t s_objects[
    OBJECT_STORE_MAX_OBJECTS
];


static size_t s_object_count =
    0;


static size_t s_selected_object =
    0;


static char s_notice[40] =
    "";


static bool s_notice_warning =
    false;


static ui_objects_list_kind_t current_list_kind(void)
{
    return
        s_mode ==
            OBJECT_MODE_SELECT_DELETE ||
        s_mode ==
            OBJECT_MODE_DELETE_CONFIRM
        ? UI_OBJECTS_LIST_DELETE
        : UI_OBJECTS_LIST_SET_ACTIVE;
}


static void select_active_object(void)
{
    s_selected_object =
        0;

    for (
        size_t i = 0;
        i < s_object_count;
        ++i
    ) {
        if (s_objects[i].active) {
            s_selected_object =
                i;

            return;
        }
    }
}


static void report_draw_error(
    esp_err_t err
)
{
    if (err == ESP_OK) {
        return;
    }

    ESP_LOGE(
        TAG,
        "Background redraw failed: %s",
        esp_err_to_name(err)
    );
}


static esp_err_t draw_menu(void)
{
    return ui_objects_view_draw_menu(
        s_selected_action,
        s_notice,
        s_notice_warning
    );
}


static esp_err_t draw_current_list(void)
{
    return ui_objects_view_draw_list(
        s_objects,
        s_object_count,
        s_selected_object,
        current_list_kind()
    );
}


static void object_service_event(
    const object_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    if (event == NULL) {
        return;
    }

    esp_err_t draw_err =
        ESP_OK;

    switch (event->type) {
        case OBJECT_SERVICE_EVENT_CREATE_DONE:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "CREATED %s",
                event->object.id
            );

            s_notice_warning =
                false;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case OBJECT_SERVICE_EVENT_CREATE_FAILED:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "CREATE FAILED"
            );

            s_notice_warning =
                true;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case OBJECT_SERVICE_EVENT_LIST_DONE:
            s_object_count =
                event->object_count;

            if (
                s_object_count >
                OBJECT_STORE_MAX_OBJECTS
            ) {
                s_object_count =
                    OBJECT_STORE_MAX_OBJECTS;
            }

            memcpy(
                s_objects,
                event->objects,
                s_object_count *
                    sizeof(
                        object_store_summary_t
                    )
            );

            if (s_object_count == 0) {
                snprintf(
                    s_notice,
                    sizeof(s_notice),
                    "NO OBJECTS"
                );

                s_notice_warning =
                    true;

                s_mode =
                    OBJECT_MODE_MENU;

                draw_err =
                    draw_menu();
                break;
            }

            select_active_object();

            s_notice[0] =
                '\0';

            s_mode =
                s_pending_list_mode;

            draw_err =
                draw_current_list();
            break;

        case OBJECT_SERVICE_EVENT_LIST_FAILED:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "LOAD FAILED"
            );

            s_notice_warning =
                true;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "ACTIVE %s",
                event->object_id
            );

            s_notice_warning =
                false;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "SAVE FAILED"
            );

            s_notice_warning =
                true;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case OBJECT_SERVICE_EVENT_DELETE_DONE:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "DELETED %s",
                event->object_id
            );

            s_notice_warning =
                false;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        case OBJECT_SERVICE_EVENT_DELETE_FAILED:
            snprintf(
                s_notice,
                sizeof(s_notice),
                "DELETE FAILED"
            );

            s_notice_warning =
                true;

            s_mode =
                OBJECT_MODE_MENU;

            draw_err =
                draw_menu();
            break;

        default:
            break;
    }

    report_draw_error(
        draw_err
    );
}


static esp_err_t create_object(void)
{
    snprintf(
        s_notice,
        sizeof(s_notice),
        "CREATING..."
    );

    s_notice_warning =
        false;

    ESP_ERROR_CHECK(
        ui_objects_view_draw_notice(
            s_notice,
            false
        )
    );

    return object_service_create_async();
}


static esp_err_t load_object_list(
    object_mode_t target_mode
)
{
    s_pending_list_mode =
        target_mode;

    snprintf(
        s_notice,
        sizeof(s_notice),
        "LOADING..."
    );

    s_notice_warning =
        false;

    ESP_ERROR_CHECK(
        ui_objects_view_draw_notice(
            s_notice,
            false
        )
    );

    return object_service_list_async();
}


static esp_err_t move_selection(
    bool down
)
{
    if (s_object_count == 0) {
        return ESP_OK;
    }

    size_t previous =
        s_selected_object;

    if (down) {
        s_selected_object++;

        if (
            s_selected_object >=
            s_object_count
        ) {
            s_selected_object =
                0;
        }
    }
    else {
        if (s_selected_object == 0) {
            s_selected_object =
                s_object_count - 1;
        }
        else {
            s_selected_object--;
        }
    }

    return ui_objects_view_update_list_selection(
        s_objects,
        s_object_count,
        previous,
        s_selected_object,
        current_list_kind()
    );
}


static esp_err_t handle_list_key(
    keypad_key_t key
)
{
    switch (key) {
        case KEYPAD_KEY_UP:
            return move_selection(
                false
            );

        case KEYPAD_KEY_DOWN:
            return move_selection(
                true
            );

        case KEYPAD_KEY_LEFT:
            s_mode =
                OBJECT_MODE_MENU;

            return draw_menu();

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            if (s_object_count == 0) {
                return ESP_OK;
            }

            if (
                s_mode ==
                OBJECT_MODE_SELECT_DELETE
            ) {
                s_mode =
                    OBJECT_MODE_DELETE_CONFIRM;

                return
                    ui_objects_view_draw_delete_confirm(
                        &s_objects[
                            s_selected_object
                        ]
                    );
            }

            ESP_ERROR_CHECK(
                ui_common_draw_footer(
                    "SAVING ACTIVE OBJECT..."
                )
            );

            return object_service_set_active_async(
                s_objects[
                    s_selected_object
                ].id
            );

        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}


static esp_err_t handle_delete_confirm(
    keypad_key_t key
)
{
    switch (key) {
        case KEYPAD_KEY_LEFT:
            s_mode =
                OBJECT_MODE_SELECT_DELETE;

            return draw_current_list();

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            ESP_ERROR_CHECK(
                ui_common_draw_footer(
                    "DELETING OBJECT..."
                )
            );

            return object_service_delete_async(
                s_objects[
                    s_selected_object
                ].id
            );

        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}


static esp_err_t handle_menu_key(
    keypad_key_t key
)
{
    switch (key) {
        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
        {
            size_t previous =
                s_selected_action;

            if (
                key ==
                KEYPAD_KEY_DOWN
            ) {
                s_selected_action =
                    (
                        s_selected_action +
                        1
                    ) %
                    UI_OBJECTS_ACTION_COUNT;
            }
            else {
                s_selected_action =
                    s_selected_action == 0
                        ? UI_OBJECTS_ACTION_COUNT - 1
                        : s_selected_action - 1;
            }

            return
                ui_objects_view_update_menu_selection(
                    previous,
                    s_selected_action
                );
        }

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            switch (s_selected_action) {
                case 0:
                    return create_object();

                case 1:
                    return load_object_list(
                        OBJECT_MODE_SELECT_ACTIVE
                    );

                case 2:
                    return load_object_list(
                        OBJECT_MODE_SELECT_DELETE
                    );

                default:
                    return ESP_ERR_INVALID_STATE;
            }

        case KEYPAD_KEY_LEFT:
            return ESP_ERR_NOT_SUPPORTED;

        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}


esp_err_t ui_objects_open(void)
{
    ESP_ERROR_CHECK(
        object_service_set_event_callback(
            object_service_event,
            NULL
        )
    );

    s_mode =
        OBJECT_MODE_MENU;

    s_selected_action =
        0;

    s_notice[0] =
        '\0';

    return draw_menu();
}


esp_err_t ui_objects_draw(void)
{
    switch (s_mode) {
        case OBJECT_MODE_SELECT_ACTIVE:
        case OBJECT_MODE_SELECT_DELETE:
            return draw_current_list();

        case OBJECT_MODE_DELETE_CONFIRM:
            return
                ui_objects_view_draw_delete_confirm(
                    &s_objects[
                        s_selected_object
                    ]
                );

        case OBJECT_MODE_MENU:
        default:
            return draw_menu();
    }
}


esp_err_t ui_objects_handle_key(
    keypad_key_t key
)
{
    if (
        object_service_is_busy()
    ) {
        return ESP_OK;
    }

    switch (s_mode) {
        case OBJECT_MODE_SELECT_ACTIVE:
        case OBJECT_MODE_SELECT_DELETE:
            return handle_list_key(
                key
            );

        case OBJECT_MODE_DELETE_CONFIRM:
            return handle_delete_confirm(
                key
            );

        case OBJECT_MODE_MENU:
        default:
            return handle_menu_key(
                key
            );
    }
}