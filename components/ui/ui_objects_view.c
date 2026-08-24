#include "ui_objects_view.h"

#include "display.h"
#include "ui_internal.h"
#include "ui_objects_view_internal.h"
#include "ui_theme.h"


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

    return
        display_draw_text_fast(
            34,
            ACTION_Y[action],
            ACTION_NAMES[action],
            foreground,
            background,
            2
        );
}


esp_err_t ui_objects_view_draw_menu(
    size_t selected_action,
    const object_service_summary_t *active_object,
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
        ui_objects_view_draw_active_line(
            active_object
        )
    );

    for (
        size_t i = 0;
        i <
            UI_OBJECTS_ACTION_COUNT;
        ++i
    ) {
        ESP_ERROR_CHECK(
            draw_action(
                i,
                i ==
                    selected_action
            )
        );
    }

    ESP_ERROR_CHECK(
        ui_objects_view_draw_notice(
            notice,
            notice_warning
        )
    );

    return
        ui_common_draw_footer(
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

    return
        draw_action(
            selected_action,
            true
        );
}
