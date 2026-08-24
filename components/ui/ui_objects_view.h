#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "object_store.h"


#define UI_OBJECTS_ACTION_COUNT 3


typedef enum
{
    UI_OBJECTS_LIST_SET_ACTIVE = 0,
    UI_OBJECTS_LIST_DELETE

} ui_objects_list_kind_t;


esp_err_t ui_objects_view_draw_menu(
    size_t selected_action,
    const char *notice,
    bool notice_warning
);


esp_err_t ui_objects_view_update_menu_selection(
    size_t previous_action,
    size_t selected_action
);


esp_err_t ui_objects_view_draw_notice(
    const char *notice,
    bool warning
);


esp_err_t ui_objects_view_draw_list(
    const object_store_summary_t *objects,
    size_t object_count,
    size_t selected_object,
    ui_objects_list_kind_t kind
);


esp_err_t ui_objects_view_update_list_selection(
    const object_store_summary_t *objects,
    size_t object_count,
    size_t previous_object,
    size_t selected_object,
    ui_objects_list_kind_t kind
);


esp_err_t ui_objects_view_draw_delete_confirm(
    const object_store_summary_t *object
);