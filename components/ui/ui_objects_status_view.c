#include "ui_objects_view.h"

#include <stdio.h>

#include "display.h"
#include "ui_objects_view_internal.h"
#include "ui_theme.h"


esp_err_t ui_objects_view_draw_active_line(
    const object_service_summary_t *active_object
)
{
    char line[48];

    if (active_object != NULL) {
        snprintf(
            line,
            sizeof(line),
            "ACTIVE: %s",
            active_object->id
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

    return
        display_draw_text_fast(
            14,
            72,
            line,
            active_object != NULL
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

    return
        display_draw_text_fast(
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
