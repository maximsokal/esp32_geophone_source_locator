#include "app_startup_status.h"

#include "app_config.h"

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
    ESP_LOGI(TAG, "Wi-Fi: OK");

#if APP_WIFI_LOCAL_ONLY_ENABLED
    ESP_LOGI(TAG, "Wi-Fi routing: LOCAL ONLY");
#else
    ESP_LOGI(TAG, "Wi-Fi routing: DEFAULT ROUTER");
#endif

    ESP_LOGI(TAG, "Web server: OK");

#if APP_CAPTIVE_PORTAL_ENABLED
    ESP_LOGI(TAG, "Captive portal: OK");
#else
    ESP_LOGI(TAG, "Captive portal: DISABLED");
#endif

    ESP_LOGI(TAG, "Seismic test pipeline: OK");
    ESP_LOGI(TAG, "UI: OK");

    ESP_LOGI(
        TAG,
        "Hardware bring-up completed"
    );
}
