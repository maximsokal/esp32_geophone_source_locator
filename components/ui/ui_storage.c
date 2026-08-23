#include "ui_internal.h"

#include "display.h"
#include "ui_theme.h"


esp_err_t ui_storage_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "STORAGE"
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            82,
            "SD CARD READY",
            UI_COLOR_SUCCESS,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            104,
            "FILE BROWSER NEXT",
            UI_COLOR_SECONDARY,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    return ui_common_draw_footer(
        "LEFT BACK"
    );
}