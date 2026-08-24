#include "ui_storage_view.h"

#include <inttypes.h>
#include <stdio.h>

#include "display.h"
#include "ui_internal.h"
#include "ui_theme.h"


static const char *ACTION_NAMES[
    UI_STORAGE_ACTION_COUNT
] = {
    "CARD INFO",
    "FREE SPACE",
    "ERASE DATA"
};


static const int ACTION_Y[
    UI_STORAGE_ACTION_COUNT
] = {
    88,
    122,
    156
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

    return display_draw_text_fast(
        34,
        ACTION_Y[action],
        ACTION_NAMES[action],
        foreground,
        background,
        2
    );
}


static uint64_t bytes_to_mb(
    uint64_t bytes
)
{
    return
        bytes /
        (1024ULL * 1024ULL);
}


esp_err_t ui_storage_view_draw_menu(
    size_t selected_action,
    const char *notice,
    bool warning
)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "STORAGE"
        )
    );

    for (
        size_t i = 0;
        i < UI_STORAGE_ACTION_COUNT;
        ++i
    ) {
        ESP_ERROR_CHECK(
            draw_action(
                i,
                i == selected_action
            )
        );
    }

    if (
        notice != NULL &&
        notice[0] != '\0'
    ) {
        ESP_ERROR_CHECK(
            display_draw_text_fast(
                14,
                194,
                notice,
                warning
                    ? UI_COLOR_WARNING
                    : UI_COLOR_SUCCESS,
                UI_COLOR_BACKGROUND,
                1
            )
        );
    }

    return ui_common_draw_footer(
        "UP/DOWN  OK OPEN  LEFT BACK"
    );
}


esp_err_t ui_storage_view_update_selection(
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

    return draw_action(
        selected_action,
        true
    );
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
        bytes_to_mb(
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

    return ui_common_draw_footer(
        "LEFT BACK"
    );
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
        bytes_to_mb(
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
        bytes_to_mb(
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
        bytes_to_mb(
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

    return ui_common_draw_footer(
        "LEFT BACK"
    );
}


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

    return ui_common_draw_footer(
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

    return ui_common_draw_footer(
        "ERASE IN PROGRESS"
    );
}