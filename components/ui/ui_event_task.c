#include "ui_internal.h"

#include <stdbool.h>

#include "app_event_bus.h"
#include "app_task_config.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "UI_EVENT";


#define UI_EVENT_RECEIVE_TIMEOUT_MS  100


static TaskHandle_t s_task =
    NULL;

static volatile bool s_running =
    false;


static void ui_event_task_main(
    void *argument
)
{
    (void)argument;

    app_event_t event;

    while (s_running) {
        esp_err_t err =
            app_event_bus_receive_timeout(
                &event,
                UI_EVENT_RECEIVE_TIMEOUT_MS
            );

        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Event receive failed: %s",
                esp_err_to_name(err)
            );

            continue;
        }

        if (!s_running) {
            break;
        }

        if (!lvgl_port_lock(250)) {
            ESP_LOGW(
                TAG,
                "LVGL lock timeout"
            );

            continue;
        }

        if (event.type == APP_EVENT_KEYPAD) {
            ui_dispatch_key_locked(
                (keypad_key_t)event.value
            );
        } else {
            ui_dispatch_app_event_locked(
                (int)event.type,
                event.value
            );
        }

        lvgl_port_unlock();
    }

    s_running =
        false;

    s_task =
        NULL;

    ESP_LOGI(
        TAG,
        "UI event task stopped"
    );

    vTaskDelete(
        NULL
    );
}


esp_err_t ui_event_task_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_running =
        true;

    BaseType_t result =
        xTaskCreatePinnedToCore(
            ui_event_task_main,
            "ui_event",
            APP_TASK_STACK_UI_EVENT,
            NULL,
            APP_TASK_PRIORITY_UI_EVENT,
            &s_task,
            APP_TASK_CORE_SERVICES
        );

    if (result != pdPASS) {
        s_running =
            false;

        s_task =
            NULL;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "UI event task started core=%d",
        APP_TASK_CORE_SERVICES
    );

    return ESP_OK;
}


void ui_event_task_stop(void)
{
    if (s_task == NULL) {
        return;
    }

    s_running =
        false;

    while (s_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }
}
