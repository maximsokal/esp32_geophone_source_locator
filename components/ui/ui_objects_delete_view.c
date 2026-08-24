#include "ui_objects_view.h"

#include "display.h"
#include "ui_internal.h"
#include "ui_theme.h"


esp_err_t ui_objects_view_draw_delete_confirm(
    const object_service_summary_t *object
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

    return
        ui_common_draw_footer(
            "OK DELETE  LEFT CANCEL"
        );
}
