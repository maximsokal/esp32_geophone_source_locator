#include "app_event_bus.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


static const char *TAG =
    "APP_EVENT_BUS";


#define APP_EVENT_QUEUE_LENGTH    24


static QueueHandle_t s_queue =
    NULL;


esp_err_t app_event_bus_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }

    s_queue =
        xQueueCreate(
            APP_EVENT_QUEUE_LENGTH,
            sizeof(app_event_t)
        );

    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Event queue initialized"
    );

    return ESP_OK;
}


esp_err_t app_event_bus_publish(
    app_event_type_t type,
    int32_t value
)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const app_event_t event = {
        .type = type,
        .value = value,
    };

    if (
        xQueueSend(
            s_queue,
            &event,
            0
        ) != pdTRUE
    ) {
        ESP_LOGW(
            TAG,
            "Event queue full, type=%d",
            (int)type
        );

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


esp_err_t app_event_bus_receive(
    app_event_t *event
)
{
    if (
        s_queue == NULL ||
        event == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        xQueueReceive(
            s_queue,
            event,
            portMAX_DELAY
        ) != pdTRUE
    ) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
