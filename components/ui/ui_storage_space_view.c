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


esp_err_t ui_storage_view_draw_free_space(
    const sd_card_info_t *info
)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "FREE SPACE"
        )
    );

    uint64_t used =
        info->total_bytes >=
                info->free_bytes
        ? info->total_bytes -
            info->free_bytes
        : 0;

    char line[64];

    snprintf(
        line,
        sizeof(line),
        "TOTAL: %" PRIu64 " MB",
        ui_storage_bytes_to_mb(
            info->total_bytes
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            82,
            line,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    snprintf(
        line,
        sizeof(line),
        "FREE: %" PRIu64 " MB",
        ui_storage_bytes_to_mb(
            info->free_bytes
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            112,
            line,
            UI_COLOR_SUCCESS,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    snprintf(
        line,
        sizeof(line),
        "USED: %" PRIu64 " MB",
        ui_storage_bytes_to_mb(
            used
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text_fast(
            18,
            142,
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
