#include "ui_storage_internal.h"

#include <stdio.h>

#include "esp_log.h"


static const char *TAG =
    "UI_STORAGE";


static void ui_storage_report_draw_error(
    esp_err_t err
)
{
    if (err == ESP_OK) {
        return;
    }

    ESP_LOGE(
        TAG,
        "Background redraw failed: %s",
        esp_err_to_name(err)
    );
}


static void ui_storage_finish_menu(
    const char *notice,
    bool warning
)
{
    ui_storage_state_t *state =
        ui_storage_state();

    snprintf(
        state->notice,
        sizeof(state->notice),
        "%s",
        notice
    );

    state->notice_warning =
        warning;

    state->mode =
        STORAGE_MODE_MENU;

    ui_storage_report_draw_error(
        ui_storage_draw_menu_internal()
    );
}


void ui_storage_service_event(
    const storage_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    if (event == NULL) {
        return;
    }

    ui_storage_state_t *state =
        ui_storage_state();

    switch (event->type) {
        case STORAGE_SERVICE_EVENT_INFO_DONE:
            state->info =
                event->info;

            state->mode =
                state->requested_mode;

            ui_storage_report_draw_error(
                ui_storage_draw_current_internal()
            );
            break;

        case STORAGE_SERVICE_EVENT_INFO_FAILED:
            ui_storage_finish_menu(
                "CARD INFO FAILED",
                true
            );
            break;

        case STORAGE_SERVICE_EVENT_ERASE_DONE:
            ui_storage_finish_menu(
                "DATA ERASED",
                false
            );
            break;

        case STORAGE_SERVICE_EVENT_ERASE_FAILED:
            ui_storage_finish_menu(
                "ERASE FAILED",
                true
            );
            break;

        default:
            break;
    }
}
