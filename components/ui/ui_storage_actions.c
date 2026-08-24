#include "ui_storage_internal.h"

#include "ui_internal.h"
#include "ui_storage_view.h"


esp_err_t ui_storage_request_info(
    storage_mode_t mode
)
{
    if (
        mode != STORAGE_MODE_CARD_INFO &&
        mode != STORAGE_MODE_FREE_SPACE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    ui_storage_state_t *state =
        ui_storage_state();

    state->requested_mode =
        mode;

    ESP_ERROR_CHECK(
        ui_common_draw_footer(
            "LOADING CARD INFO..."
        )
    );

    return
        storage_service_info_async();
}


esp_err_t ui_storage_start_erase(void)
{
    ui_storage_state_t *state =
        ui_storage_state();

    state->mode =
        STORAGE_MODE_ERASING;

    ESP_ERROR_CHECK(
        ui_storage_view_draw_erasing()
    );

    esp_err_t err =
        storage_service_erase_data_async();

    if (err != ESP_OK) {
        state->mode =
            STORAGE_MODE_ERASE_CONFIRM;
    }

    return err;
}
