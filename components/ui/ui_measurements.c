#include "ui_internal.h"

#include "display.h"
#include "ui_theme.h"


esp_err_t ui_measurements_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "MEASUREMENTS"
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            82,
            "NO SENSOR DATA YET",
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            104,
            "AD7606 NOT CONNECTED",
            UI_COLOR_SECONDARY,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    return ui_common_draw_footer(
        "LEFT BACK"
    );
}