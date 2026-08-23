#include "ui_internal.h"

#include "display.h"
#include "ui_theme.h"


esp_err_t ui_system_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "SYSTEM"
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            78,
            "MCU: ESP32-S3",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            98,
            "FLASH: 16 MB",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            118,
            "PSRAM: 8 MB",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            14,
            148,
            "TFT: ILI9341 320X240",
            UI_COLOR_SECONDARY,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    return ui_common_draw_footer(
        "LEFT BACK"
    );
}