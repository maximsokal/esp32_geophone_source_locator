#include "ui_storage_internal.h"

#include "ui_storage_view.h"


esp_err_t ui_storage_draw_menu_internal(void)
{
    ui_storage_state_t *state =
        ui_storage_state();

    return
        ui_storage_view_draw_menu(
            state->selected_action,
            state->notice,
            state->notice_warning
        );
}


esp_err_t ui_storage_draw_current_internal(void)
{
    ui_storage_state_t *state =
        ui_storage_state();

    switch (state->mode) {
        case STORAGE_MODE_CARD_INFO:
            return
                ui_storage_view_draw_card_info(
                    &state->info
                );

        case STORAGE_MODE_FREE_SPACE:
            return
                ui_storage_view_draw_free_space(
                    &state->info
                );

        case STORAGE_MODE_ERASE_CONFIRM:
            return
                ui_storage_view_draw_erase_confirm();

        case STORAGE_MODE_ERASING:
            return
                ui_storage_view_draw_erasing();

        case STORAGE_MODE_MENU:
        default:
            return
                ui_storage_draw_menu_internal();
    }
}
