#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "sd_card.h"


#define UI_STORAGE_ACTION_COUNT 3


esp_err_t ui_storage_view_draw_menu(
    size_t selected_action,
    const char *notice,
    bool warning
);


esp_err_t ui_storage_view_update_selection(
    size_t previous_action,
    size_t selected_action
);


esp_err_t ui_storage_view_draw_card_info(
    const sd_card_info_t *info
);


esp_err_t ui_storage_view_draw_free_space(
    const sd_card_info_t *info
);


esp_err_t ui_storage_view_draw_erase_confirm(void);


esp_err_t ui_storage_view_draw_erasing(void);