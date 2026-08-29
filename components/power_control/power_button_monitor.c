#include "power_button_monitor.h"

#include <stddef.h>

#include "app_task_config.h"

#include "esp_log.h"

#include "power_button_input.h"
#include "power_button_monitor_internal.h"


static const char *TAG =
    "POWER_BUTTON";


esp_err_t power_button_monitor_init(void)
{
    power_button_monitor_state_t *state =
        power_button_monitor_state();

    if (state->initialized) {
        return ESP_OK;
    }

    esp_err_t err =
        power_button_input_init();

    if (err != ESP_OK) {
        return err;
    }

    state->events =
        xEventGroupCreate();

    if (state->events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xEventGroupClearBits(
        state->events,
        POWER_EVENT_SHUTDOWN_REQUESTED
    );

    state->running =
        true;

    BaseType_t task_result =
        xTaskCreatePinnedToCore(
            power_button_monitor_worker,
            "power_button",
            APP_TASK_STACK_POWER_BUTTON,
            NULL,
            APP_TASK_PRIORITY_POWER_BUTTON,
            &state->task,
            APP_TASK_CORE_SERVICES
        );

    if (
        task_result !=
        pdPASS
    ) {
        state->running =
            false;

        state->task =
            NULL;

        vEventGroupDelete(
            state->events
        );

        state->events =
            NULL;

        ESP_LOGE(
            TAG,
            "Failed to create power button monitor"
        );

        return ESP_ERR_NO_MEM;
    }

    state->initialized =
        true;

    ESP_LOGI(
        TAG,
        "Monitor task core=%d",
        APP_TASK_CORE_SERVICES
    );

    return ESP_OK;
}


void power_button_monitor_stop(void)
{
    power_button_monitor_state_t *state =
        power_button_monitor_state();

    if (!state->initialized) {
        return;
    }

    state->running =
        false;

    while (
        state->task !=
        NULL
    ) {
        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }
}
