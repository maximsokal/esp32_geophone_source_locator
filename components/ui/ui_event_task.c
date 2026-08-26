#include "ui_event_task.h"

#include "app_event_bus.h"
#include "ui_event_dispatch.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "UI_EVENT";


static TaskHandle_t s_task =
    NULL;


static void ui_event_task_main(
    void *argument
)
{
    (void)argument;

    app_event_t event;

    while (true) {
        esp_err_t err =
            app_event_bus_receive(
                &event
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Event receive failed: %s",
                esp_err_to_name(err)
            );

            continue;
        }

        if (
            event.type ==
            APP_EVENT_KEYPAD
        ) {
            err =
                ui_event_dispatch_key(
                    (keypad_key_t)
                        event.value
                );
        } else {
            err =
                ui_event_dispatch_app(
                    event.type
                );
        }

        if (
            err != ESP_OK &&
            err != ESP_ERR_NOT_SUPPORTED
        ) {
            ESP_LOGE(
                TAG,
                "Event dispatch failed: %s",
                esp_err_to_name(err)
            );
        }
    }
}


esp_err_t ui_event_task_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t result =
        xTaskCreate(
            ui_event_task_main,
            "ui_event",
            6144,
            NULL,
            6,
            &s_task
        );

    if (result != pdPASS) {
        s_task =
            NULL;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "UI event task started"
    );

    return ESP_OK;
}
