#include "power_button_monitor_internal.h"

#include <stddef.h>

#include "esp_log.h"

#include "power_button_input.h"

#include "runtime_observability.h"


static const char *TAG =
    "POWER_BUTTON";


static bool power_button_wait_for_startup_release(void)
{
    if (
        power_button_input_is_high()
    ) {
        ESP_LOGI(
            TAG,
            "Waiting for startup button release"
        );
    }

    return
        power_button_wait_for_level(
            false,
            BUTTON_RELEASE_CONFIRM_MS,
            true
        );
}


void power_button_monitor_worker(
    void *argument
)
{
    (void)argument;

    power_button_monitor_state_t *state =
        power_button_monitor_state();

    if (
        !power_button_wait_for_startup_release()
    ) {
        state->task =
            NULL;

        vTaskDelete(
            NULL
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Power button armed"
    );

    const unsigned required_high_samples =
        power_button_required_samples(
            BUTTON_PRESS_CONFIRM_MS
        );

    unsigned high_samples =
        0;

    uint64_t poll_count = 0U;

    while (state->running) {
        if (
            power_button_input_is_high()
        ) {
            high_samples++;

            if (
                high_samples >=
                required_high_samples
            ) {
                xEventGroupSetBits(
                    state->events,
                    POWER_EVENT_SHUTDOWN_REQUESTED
                );

                ESP_LOGI(
                    TAG,
                    "Shutdown request confirmed after %u ms HIGH",
                    BUTTON_PRESS_CONFIRM_MS
                );

                break;
            }
        }
        else {
            high_samples =
                0;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                BUTTON_POLL_MS
            )
        );

        runtime_observability_beat(
            RUNTIME_TASK_POWER_BUTTON,
            ++poll_count
        );
    }

    state->running =
        false;

    state->task =
        NULL;

    vTaskDelete(
        NULL
    );
}
