#include "app_startup_status.h"

#include "wifi_manager.h"

#include "esp_log.h"


static const char *TAG =
    "APP";


void app_startup_log_success(void)
{
    ESP_LOGI(TAG, "Display: OK");
    ESP_LOGI(TAG, "SD card: OK");
    ESP_LOGI(TAG, "Storage manager: OK");
    ESP_LOGI(TAG, "Storage service: OK");
    ESP_LOGI(TAG, "Object recovery: OK");
    ESP_LOGI(TAG, "Object store: OK");
    ESP_LOGI(TAG, "Object service: OK");
    ESP_LOGI(TAG, "Keypad: OK");

    if (
        wifi_manager_is_setup_mode()
    ) {
        ESP_LOGI(
            TAG,
            "Wi-Fi mode: SETUP AP"
        );

        ESP_LOGI(
            TAG,
            "Setup page: LOCAL QR FLOW"
        );
    } else {
        ESP_LOGI(
            TAG,
            "Wi-Fi mode: STA"
        );
    }

    ESP_LOGI(TAG, "Web server: OK");
    ESP_LOGI(TAG, "Seismic test pipeline: OK");
    ESP_LOGI(TAG, "UI: OK");

    ESP_LOGI(
        TAG,
        "Hardware bring-up completed"
    );
}
