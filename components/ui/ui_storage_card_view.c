#include "ui_storage_view.h"

#include <inttypes.h>
#include <stdio.h>

#include "display.h"
#include "ui_internal.h"
#include "ui_theme.h"


static uint64_t ui_storage_bytes_to_mb(
    uint64_t bytes
)
{
    return
        bytes /
        (1024ULL * 1024ULL);
}


esp_err_t ui_storage_view_draw_card_info(
    const sd_card_info_t *info
)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "CARD INFO"
        )
    );

    char line[64];

    snprintf(
        line,
        sizeof(line),
        "NAME: %s",
        info->name
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            78,
            line,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    snprintf(
        line,
        sizeof(line),
        "TYPE: %s",
        info->type
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            102,
            line,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    snprintf(
        line,
        sizeof(line),
        "SIZE: %" PRIu64 " MB",
        ui_storage_bytes_to_mb(
            info->capacity_bytes
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            126,
            line,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    snprintf(
        line,
        sizeof(line),
        "SPEED: %lu.%02lu MHZ",
        (unsigned long)(
            info->speed_khz /
            1000
        ),
        (unsigned long)(
            (
                info->speed_khz %
                1000
            ) /
            10
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            150,
            line,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    return
        ui_common_draw_footer(
            "LEFT BACK"
        );
}
