#include "keypad_internal.h"

#include <stddef.h>

#include "app_task_config.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "keypad_event_internal.h"


static const char *TAG =
    "KEYPAD";


static keypad_event_state_t s_state = {
    .task =
        NULL,

    .callback =
        NULL,

    .user_context =
        NULL,

    .running =
        false,
};


keypad_event_state_t *keypad_event_state(void)
{
    return
        &s_state;
}


esp_err_t keypad_event_monitor_start(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (s_state.task != NULL) {
        ESP_LOGW(
            TAG,
            "Key event monitor already running"
        );

        return ESP_OK;
    }

    s_state.callback =
        callback;

    s_state.user_context =
        user_context;

    s_state.running =
        true;

    BaseType_t result =
        xTaskCreatePinnedToCore(
            keypad_event_worker,
            "keypad_events",
            APP_TASK_STACK_KEYPAD,
            NULL,
            APP_TASK_PRIORITY_KEYPAD,
            &s_state.task,
            APP_TASK_CORE_SERVICES
        );

    if (
        result !=
        pdPASS
    ) {
        s_state.task =
            NULL;

        s_state.running =
            false;

        s_state.callback =
            NULL;

        s_state.user_context =
            NULL;

        ESP_LOGE(
            TAG,
            "Failed to create keypad event task"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Key event monitor core=%d",
        APP_TASK_CORE_SERVICES
    );

    return ESP_OK;
}


void keypad_event_monitor_stop(void)
{
    if (s_state.task == NULL) {
        return;
    }

    s_state.running =
        false;

    while (
        s_state.task !=
        NULL
    ) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    s_state.callback =
        NULL;

    s_state.user_context =
        NULL;
}
