#include "ui_objects_view.h"

#include <stdio.h>

#include "display.h"
#include "object_store.h"
#include "ui_internal.h"
#include "ui_theme.h"


#define OBJECT_LIST_VISIBLE      5

#define OBJECT_LIST_X            18
#define OBJECT_LIST_Y            67
#define OBJECT_LIST_WIDTH        284
#define OBJECT_LIST_HEIGHT       140
#define OBJECT_ROW_Y             70
#define OBJECT_ROW_STEP          28
#define OBJECT_ROW_HEIGHT        22


static const char *ACTION_NAMES[
    UI_OBJECTS_ACTION_COUNT
] = {
    "NEW OBJECT",
    "SELECT OBJECT",
    "DELETE OBJECT"
};


static const int ACTION_Y[
    UI_OBJECTS_ACTION_COUNT
] = {
    96,
    128,
    160
};


static esp_err_t draw_action(
    size_t action,
    bool selected
)
{
    uint16_t foreground =
        selected
            ? DISPLAY_COLOR_BLACK
            : UI_COLOR_TEXT;

    uint16_t background =
        selected
            ? UI_COLOR_ACCENT
            : UI_COLOR_BACKGROUND;

    ESP_ERROR_CHECK(
        display_fill_rect(
            22,
            ACTION_Y[action] - 4,
            276,
            26,
            background
        )
    );

    return display_draw_text_fast(
        34,
        ACTION_Y[action],
        ACTION_NAMES[action],
        foreground,
        background,
        2
    );
}


static esp_err_t draw_active_line(void)
{
    object_store_summary_t active;

    esp_err_t active_err =
        object_store_get_active(
            &active
        );

    char line[48];

    if (active_err == ESP_OK) {
        snprintf(
            line,
            sizeof(line),
            "ACTIVE: %s",
            active.id
        );
    }
    else {
        snprintf(
            line,
            sizeof(line),
            "ACTIVE: NONE"
        );
    }

    ESP_ERROR_CHECK(
        display_fill_rect(
            14,
            68,
            292,
            14,
            UI_COLOR_BACKGROUND
        )
    );

    return display_draw_text_fast(
        14,
        72,
        line,
        active_err == ESP_OK
            ? UI_COLOR_SUCCESS
            : UI_COLOR_SECONDARY,
        UI_COLOR_BACKGROUND,
        1
    );
}


esp_err_t ui_objects_view_draw_notice(
    const char *notice,
    bool warning
)
{
    ESP_ERROR_CHECK(
        display_fill_rect(
            14,
            188,
            292,
            18,
            UI_COLOR_BACKGROUND
        )
    );

    if (
        notice == NULL ||
        notice[0] == '\0'
    ) {
        return ESP_OK;
    }

    return display_draw_text_fast(
        14,
        192,
        notice,
        warning
            ? UI_COLOR_WARNING
            : UI_COLOR_SUCCESS,
        UI_COLOR_BACKGROUND,
        1
    );
}


esp_err_t ui_objects_view_draw_menu(
    size_t selected_action,
    const char *notice,
    bool notice_warning
)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "OBJECTS"
        )
    );

    ESP_ERROR_CHECK(
        draw_active_line()
    );

    for (
        size_t i = 0;
        i < UI_OBJECTS_ACTION_COUNT;
        ++i
    ) {
        ESP_ERROR_CHECK(
            draw_action(
                i,
                i == selected_action
            )
        );
    }

    ESP_ERROR_CHECK(
        ui_objects_view_draw_notice(
            notice,
            notice_warning
        )
    );

    return ui_common_draw_footer(
        "UP/DOWN  OK OPEN  LEFT BACK"
    );
}


esp_err_t ui_objects_view_update_menu_selection(
    size_t previous_action,
    size_t selected_action
)
{
    ESP_ERROR_CHECK(
        draw_action(
            previous_action,
            false
        )
    );

    return draw_action(
        selected_action,
        true
    );
}


static size_t list_start(
    size_t object_count,
    size_t selected
)
{
    if (
        object_count <=
        OBJECT_LIST_VISIBLE
    ) {
        return 0;
    }

    size_t start =
        selected > 2
            ? selected - 2
            : 0;

    if (
        start +
            OBJECT_LIST_VISIBLE >
        object_count
    ) {
        start =
            object_count -
            OBJECT_LIST_VISIBLE;
    }

    return start;
}


static esp_err_t draw_list_row(
    const object_store_summary_t *objects,
    size_t object_count,
    size_t start,
    size_t index,
    size_t selected
)
{
    if (
        index >= object_count ||
        index < start ||
        index >=
            start +
            OBJECT_LIST_VISIBLE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t row =
        index - start;

    bool highlighted =
        index == selected;

    uint16_t foreground =
        highlighted
            ? DISPLAY_COLOR_BLACK
            : UI_COLOR_TEXT;

    uint16_t background =
        highlighted
            ? UI_COLOR_ACCENT
            : UI_COLOR_BACKGROUND;

    int y =
        OBJECT_ROW_Y +
        (int)row *
            OBJECT_ROW_STEP;

    ESP_ERROR_CHECK(
        display_fill_rect(
            OBJECT_LIST_X,
            y - 3,
            OBJECT_LIST_WIDTH,
            OBJECT_ROW_HEIGHT,
            background
        )
    );

    char line[32];

    snprintf(
        line,
        sizeof(line),
        "%c %s",
        objects[index].active
            ? '>'
            : ' ',
        objects[index].id
    );

    return display_draw_text_fast(
        28,
        y,
        line,
        foreground,
        background,
        2
    );
}


static esp_err_t draw_list_rows(
    const object_store_summary_t *objects,
    size_t object_count,
    size_t selected
)
{
    size_t start =
        list_start(
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
            draw_list_row(
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
    const object_store_summary_t *objects,
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

        return ui_common_draw_footer(
            "LEFT BACK"
        );
    }

    ESP_ERROR_CHECK(
        draw_list_rows(
            objects,
            object_count,
            selected_object
        )
    );

    return ui_common_draw_footer(
        kind ==
            UI_OBJECTS_LIST_DELETE
        ? "UP/DOWN SELECT  OK DELETE  LEFT BACK"
        : "UP/DOWN SELECT  OK SET  LEFT BACK"
    );
}


esp_err_t ui_objects_view_update_list_selection(
    const object_store_summary_t *objects,
    size_t object_count,
    size_t previous_object,
    size_t selected_object,
    ui_objects_list_kind_t kind
)
{
    (void)kind;

    size_t previous_start =
        list_start(
            object_count,
            previous_object
        );

    size_t new_start =
        list_start(
            object_count,
            selected_object
        );

    if (
        previous_start !=
        new_start
    ) {
        return draw_list_rows(
            objects,
            object_count,
            selected_object
        );
    }

    ESP_ERROR_CHECK(
        draw_list_row(
            objects,
            object_count,
            new_start,
            previous_object,
            selected_object
        )
    );

    return draw_list_row(
        objects,
        object_count,
        new_start,
        selected_object,
        selected_object
    );
}


esp_err_t ui_objects_view_draw_delete_confirm(
    const object_store_summary_t *object
)
{
    if (object == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "DELETE OBJECT"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            20,
            82,
            object->id,
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            20,
            118,
            "ERASE ALL OBJECT DATA?",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    if (object->active) {
        ESP_ERROR_CHECK(
            display_draw_text_fast(
                20,
                144,
                "THIS IS THE ACTIVE OBJECT",
                UI_COLOR_WARNING,
                UI_COLOR_BACKGROUND,
                1
            )
        );
    }

    return ui_common_draw_footer(
        "OK DELETE  LEFT CANCEL"
    );
}