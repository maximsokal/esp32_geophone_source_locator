#include "ui_storage_view.h"

#include "display.h"
#include "ui_internal.h"
#include "ui_theme.h"


esp_err_t ui_storage_view_draw_erase_confirm(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "ERASE DATA"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            82,
            "ERASE ALL APP DATA?",
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            112,
            "OBJECTS AND SESSIONS",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            136,
            "WILL BE DELETED",
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    return
        ui_common_draw_footer(
            "OK ERASE  LEFT CANCEL"
        );
}


esp_err_t ui_storage_view_draw_erasing(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "ERASE DATA"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            34,
            88,
            "ERASING...",
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            34,
            128,
            "PLEASE WAIT",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            34,
            152,
            "DO NOT REMOVE POWER",
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    return
        ui_common_draw_footer(
            "ERASE IN PROGRESS"
        );
}
