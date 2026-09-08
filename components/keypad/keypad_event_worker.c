#include "keypad_event_internal.h"

#include <stddef.h>

#include "esp_log.h"

#include "keypad.h"

#include "runtime_observability.h"


static const char *TAG =
    "KEYPAD";


static void keypad_event_deliver(
    keypad_event_state_t *state,
    keypad_key_t stable_key
)
{
    if (
        stable_key ==
        KEYPAD_KEY_NONE
    ) {
        ESP_LOGI(
            TAG,
            "Key released"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Key pressed: %s",
        keypad_key_name(
            stable_key
        )
    );

    if (state->callback != NULL) {
        state->callback(
            stable_key,
            state->user_context
        );
    }
}


static void keypad_event_update_candidate(
    keypad_key_t current_key,
    keypad_key_t *candidate_key,
    int *candidate_count
)
{
    if (
        current_key ==
        *candidate_key
    ) {
        (*candidate_count)++;

        return;
    }

    *candidate_key =
        current_key;

    *candidate_count =
        1;
}


void keypad_event_worker(
    void *arg
)
{
    (void)arg;

    keypad_event_state_t *state =
        keypad_event_state();

    keypad_key_t stable_key =
        KEYPAD_KEY_NONE;

    keypad_key_t candidate_key =
        KEYPAD_KEY_NONE;

    int candidate_count =
        0;

    uint64_t poll_count = 0U;

    ESP_LOGI(
        TAG,
        "Key event monitor started"
    );

    while (state->running) {
        keypad_key_t current_key =
            KEYPAD_KEY_NONE;

        esp_err_t err =
            keypad_read_key(
                &current_key
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to read keypad: %s",
                esp_err_to_name(err)
            );

            vTaskDelay(
                pdMS_TO_TICKS(
                    KEYPAD_POLL_PERIOD_MS
                )
            );

            runtime_observability_beat(
                RUNTIME_TASK_KEYPAD,
                ++poll_count
            );

            continue;
        }

        keypad_event_update_candidate(
            current_key,
            &candidate_key,
            &candidate_count
        );

        if (
            candidate_count >=
                KEYPAD_DEBOUNCE_COUNT &&
            candidate_key !=
                stable_key
        ) {
            stable_key =
                candidate_key;

            candidate_count =
                0;

            keypad_event_deliver(
                state,
                stable_key
            );
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                KEYPAD_POLL_PERIOD_MS
            )
        );

        runtime_observability_beat(
            RUNTIME_TASK_KEYPAD,
            ++poll_count
        );
    }

    ESP_LOGI(
        TAG,
        "Key event monitor stopped"
    );

    state->task =
        NULL;

    vTaskDelete(
        NULL
    );
}
