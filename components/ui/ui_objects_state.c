#include "ui_objects_internal.h"

#include "esp_log.h"


static const char *TAG =
    "UI_OBJECTS";


static ui_objects_state_t s_state = {
    .mode =
        OBJECT_MODE_MENU,

    .pending_list_mode =
        OBJECT_MODE_SELECT_ACTIVE,

    .selected_action =
        0,

    .object_count =
        0,

    .selected_object =
        0,

    .notice = "",

    .notice_warning =
        false,
};


ui_objects_state_t *ui_objects_state(void)
{
    return
        &s_state;
}


ui_objects_list_kind_t ui_objects_current_list_kind(void)
{
    return
        s_state.mode ==
            OBJECT_MODE_SELECT_DELETE ||
        s_state.mode ==
            OBJECT_MODE_DELETE_CONFIRM
        ? UI_OBJECTS_LIST_DELETE
        : UI_OBJECTS_LIST_SET_ACTIVE;
}


void ui_objects_select_active_object(void)
{
    s_state.selected_object =
        0;

    for (
        size_t i = 0;
        i < s_state.object_count;
        ++i
    ) {
        if (
            s_state.objects[i].active
        ) {
            s_state.selected_object =
                i;

            return;
        }
    }
}


void ui_objects_report_draw_error(
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
