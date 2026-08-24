#include "ui_objects_view.h"

#include "display.h"
#include "ui_internal.h"
#include "ui_objects_view_internal.h"
#include "ui_theme.h"


esp_err_t ui_objects_view_draw_list_rows(
    const object_service_summary_t *objects,
    size_t object_count,
    size_t selected
)
{
    size_t start =
        ui_objects_view_list_start(
            object_count,
            selected
        );

    ESP_ERROR_CHECK(
        display_fill_rect(
            OBJECT_LIST_X,
            OBJECT_LIST_Y,
            OBJECT_LIST_WIDTH,
            OBJECT_LIST_HEIGHT,
            UI_COLOR_BACKGROUND
        )
    );

    size_t end =
        start +
        OBJECT_LIST_VISIBLE;

    if (end > object_count) {
        end =
            object_count;
    }

    for (
        size_t index = start;
        index < end;
        ++index
    ) {
        ESP_ERROR_CHECK(
            ui_objects_view_draw_list_row(
                objects,
                object_count,
                start,
                index,
                selected
            )
        );
    }

    return ESP_OK;
}


esp_err_t ui_objects_view_draw_list(
    const object_service_summary_t *objects,
    size_t object_count,
    size_t selected_object,
    ui_objects_list_kind_t kind
)
{
    const char *title =
        kind ==
            UI_OBJECTS_LIST_DELETE
        ? "DELETE OBJECT"
        : "SELECT OBJECT";

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            title
        )
    );

    if (object_count == 0) {
        ESP_ERROR_CHECK(
            display_draw_text_fast(
                14,
                90,
                "NO OBJECTS",
                UI_COLOR_WARNING,
                UI_COLOR_BACKGROUND,
                2
            )
        );

        return
            ui_common_draw_footer(
                "LEFT BACK"
            );
    }

    ESP_ERROR_CHECK(
        ui_objects_view_draw_list_rows(
            objects,
            object_count,
            selected_object
        )
    );

    return
        ui_common_draw_footer(
            kind ==
                UI_OBJECTS_LIST_DELETE
            ? "UP/DOWN SELECT  OK DELETE  LEFT BACK"
            : "UP/DOWN SELECT  OK SET  LEFT BACK"
        );
}


esp_err_t ui_objects_view_update_list_selection(
    const object_service_summary_t *objects,
    size_t object_count,
    size_t previous_object,
    size_t selected_object,
    ui_objects_list_kind_t kind
)
{
    (void)kind;

    size_t previous_start =
        ui_objects_view_list_start(
            object_count,
            previous_object
        );

    size_t new_start =
        ui_objects_view_list_start(
            object_count,
            selected_object
        );

    if (
        previous_start !=
        new_start
    ) {
        return
            ui_objects_view_draw_list_rows(
                objects,
                object_count,
                selected_object
            );
    }

    ESP_ERROR_CHECK(
        ui_objects_view_draw_list_row(
            objects,
            object_count,
            new_start,
            previous_object,
            selected_object
        )
    );

    return
        ui_objects_view_draw_list_row(
            objects,
            object_count,
            new_start,
            selected_object,
            selected_object
        );
}
