#include "ui_objects_view_internal.h"

#include <stdbool.h>
#include <stdio.h>

#include "display.h"
#include "ui_theme.h"


size_t ui_objects_view_list_start(
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


esp_err_t ui_objects_view_draw_list_row(
    const object_service_summary_t *objects,
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
        index -
        start;

    bool highlighted =
        index ==
        selected;

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

    return
        display_draw_text_fast(
            28,
            y,
            line,
            foreground,
            background,
            2
        );
}
