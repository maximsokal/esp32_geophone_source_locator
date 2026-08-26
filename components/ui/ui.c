#include "ui.h"

#include "esp_log.h"

#include "ui_event_task.h"
#include "ui_screen_router.h"
#include "ui_state.h"


static const char *TAG =
    "UI";


esp_err_t ui_init(void)
{
    ui_state_reset();

    ESP_LOGI(
        TAG,
        "Initializing UI"
    );

    esp_err_t err =
        ui_screen_draw(
            UI_SCREEN_HOME
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize UI: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        ui_event_task_start();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start UI event task: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "UI ready"
    );

    return ESP_OK;
}
