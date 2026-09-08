#include "ui_internal.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_event_bus.h"
#include "app_task_config.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "UI_EVENT";


#define UI_EVENT_RECEIVE_TIMEOUT_MS      100U
#define UI_EVENT_LVGL_LOCK_SLICE_MS      100U
#define UI_EVENT_LVGL_WARN_INTERVAL_MS  1000U


static TaskHandle_t s_task =
    NULL;

static volatile bool s_running =
    false;


/*
 * Storage and TFT share SPI2. While an SD operation is active the display
 * guard intentionally holds the LVGL mutex so no new LCD transfer can be
 * queued. A keypad event must therefore wait for that bounded storage window
 * instead of being discarded after one short lock timeout.
 *
 * Use short lock slices so shutdown can still stop this task promptly.
 */
static bool ui_event_lock_lvgl(void)
{
    uint32_t waited_ms = 0U;

    while (s_running) {
        if (lvgl_port_lock(UI_EVENT_LVGL_LOCK_SLICE_MS)) {
            return true;
        }

        waited_ms += UI_EVENT_LVGL_LOCK_SLICE_MS;
        if (waited_ms >= UI_EVENT_LVGL_WARN_INTERVAL_MS) {
            ESP_LOGW(
                TAG,
                "Waiting for LVGL lock (%u ms); preserving queued UI event",
                (unsigned)waited_ms
            );
            waited_ms = 0U;
        }
    }

    return false;
}


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

        if (!ui_event_lock_lvgl()) {
            break;
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
