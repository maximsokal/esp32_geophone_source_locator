#include "app_shutdown.h"
#include "app_startup.h"

#include "esp_log.h"


static const char *TAG =
    "APP";


void app_main(void)
{
    ESP_LOGI(
        TAG,
        "ESP32 Geophone Source Locator"
    );

    app_startup_run();

    app_shutdown_wait_and_run();
}
