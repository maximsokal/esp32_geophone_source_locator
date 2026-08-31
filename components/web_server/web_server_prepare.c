#include "web_server.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "web_server_internal.h"


#define WEB_SERVER_PREPARE_STACK_BYTES  8192U
#define WEB_SERVER_PREPARE_PRIORITY     4U


static const char *TAG =
    "WEB_PREPARE";


typedef struct
{
    TaskHandle_t waiter;
    esp_err_t result;

} web_server_prepare_context_t;


static void web_server_prepare_task(
    void *argument
)
{
    web_server_prepare_context_t *context =
        (web_server_prepare_context_t *)argument;

    if (context == NULL) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(
        TAG,
        "Preparing React Web UI before network startup"
    );

    context->result =
        web_webui_prepare();

    ESP_LOGI(
        TAG,
        "React Web UI preparation finished: %s",
        esp_err_to_name(context->result)
    );

    xTaskNotifyGive(
        context->waiter
    );

    vTaskDelete(NULL);
}


esp_err_t web_server_prepare_webui(void)
{
    web_server_prepare_context_t context = {
        .waiter = xTaskGetCurrentTaskHandle(),
        .result = ESP_ERR_INVALID_STATE,
    };

    TaskHandle_t prepare_task =
        NULL;

    const BaseType_t created =
        xTaskCreate(
            web_server_prepare_task,
            "webui_seed",
            WEB_SERVER_PREPARE_STACK_BYTES,
            &context,
            WEB_SERVER_PREPARE_PRIORITY,
            &prepare_task
        );

    if (created != pdPASS) {
        ESP_LOGE(
            TAG,
            "Could not create Web UI preparation task"
        );

        return ESP_ERR_NO_MEM;
    }

    /*
     * The worker intentionally runs before Wi-Fi starts. Waiting here keeps
     * all FAT/SD work out of ESP-IDF's sys_evt task and ensures the temporary
     * 8 KiB worker stack is released before HTTPS/TLS allocations begin.
     */
    (void)ulTaskNotifyTake(
        pdTRUE,
        portMAX_DELAY
    );

    return context.result;
}
