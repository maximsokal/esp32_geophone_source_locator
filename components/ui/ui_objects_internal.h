#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "keypad.h"
#include "object_service.h"
#include "ui_objects_view.h"


typedef enum
{
    OBJECT_MODE_MENU = 0,
    OBJECT_MODE_SELECT_ACTIVE,
    OBJECT_MODE_SELECT_DELETE,
    OBJECT_MODE_DELETE_CONFIRM

} object_mode_t;


typedef struct
{
    object_mode_t mode;

    object_mode_t pending_list_mode;

    size_t selected_action;

    object_service_summary_t objects[
        OBJECT_SERVICE_MAX_OBJECTS
    ];

    size_t object_count;

    size_t selected_object;

    char notice[40];

    bool notice_warning;

} ui_objects_state_t;


ui_objects_state_t *ui_objects_state(void);


ui_objects_list_kind_t ui_objects_current_list_kind(void);


void ui_objects_select_active_object(void);


void ui_objects_report_draw_error(
    esp_err_t err
);


esp_err_t ui_objects_draw_menu_internal(void);


esp_err_t ui_objects_draw_current_list_internal(void);


void ui_objects_service_event(
    const object_service_event_t *event,
    void *user_context
);


void ui_objects_event_handle_list(
    const object_service_event_t *event
);


esp_err_t ui_objects_handle_menu_key_internal(
    keypad_key_t key
);


esp_err_t ui_objects_handle_list_key_internal(
    keypad_key_t key
);


esp_err_t ui_objects_handle_delete_confirm_internal(
    keypad_key_t key
);
