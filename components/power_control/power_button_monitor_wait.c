#include "power_button_monitor.h"

#include <stddef.h>

#include "esp_log.h"

#include "power_button_input.h"
#include "power_button_monitor_internal.h"


static const char *TAG =
    "POWER_BUTTON";


bool power_button_wait_for_level(
    bool high,
    unsigned duration_ms,
    bool stop_sensitive
)
{
    power_button_monitor_state_t *state =
        power_button_monitor_state();

    const unsigned required =
        power_button_required_samples(
            duration_ms
        );

    unsigned stable_samples =
        0;

    while (
        stable_samples <
        required
    ) {
        if (
            stop_sensitive &&
            !state->running
        ) {
            return false;
        }

        if (
            power_button_input_is_high() ==
            high
        ) {
            stable_samples++;
        }
        else {
            stable_samples =
                0;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );
    }

    return true;
}


void power_button_monitor_wait_for_shutdown_request(void)
{
    power_button_monitor_state_t *state =
        power_button_monitor_state();

    if (
        !state->initialized ||
        state->events == NULL
    ) {
        return;
    }

    xEventGroupWaitBits(
        state->events,
        POWER_EVENT_SHUTDOWN_REQUESTED,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(
        TAG,
        "Shutdown button latched"
    );

    (void)
        power_button_wait_for_level(
            false,
            BUTTON_RELEASE_CONFIRM_MS,
            false
        );

    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );
}


bool power_button_monitor_is_shutdown_requested(void)
{
    power_button_monitor_state_t *state =
        power_button_monitor_state();

    if (
        !state->initialized ||
        state->events == NULL
    ) {
        return false;
    }

    EventBits_t bits =
        xEventGroupGetBits(
            state->events
        );

    return
        (
            bits &
            POWER_EVENT_SHUTDOWN_REQUESTED
        ) != 0;
}
