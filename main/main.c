#include "app_shutdown.h"
#include "app_startup.h"
#include "nrf_topology_service.h"

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

    /*
     * Topology discovery is deliberately outside app_startup_run(): the local
     * TFT/keypad interactive boundary and startup timing are already complete.
     * The service itself is asynchronous and optional during hardware bring-up.
     */
    const esp_err_t topology_err =
        nrf_topology_service_start();
    if (topology_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "nRF topology service unavailable: %s",
            esp_err_to_name(topology_err)
        );
    }

    app_shutdown_wait_and_run();
}
