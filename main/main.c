#include "app_pod_status_bridge.h"
#include "app_shutdown.h"
#include "app_startup.h"
#include "nrf_emergency_stop_monitor.h"
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

    /*
     * Whole-object emergency-stop watch (MAX-118): reads the same topology
     * sensor cache, so it is started right after that service, also
     * non-fatal to boot.
     */
    const esp_err_t estop_err =
        nrf_emergency_stop_monitor_start();
    if (estop_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "nRF emergency-stop monitor unavailable: %s",
            esp_err_to_name(estop_err)
        );
    }

    /*
     * MAX-118: surface the barometer data nrf_topology_service already
     * caches (MAX-114) through device_status, so the Measurements screen and
     * Web status API have live per-pod altitude to show. Non-fatal like the
     * other topology-cache consumers above.
     */
    const esp_err_t pod_status_bridge_err =
        app_pod_status_bridge_start();
    if (pod_status_bridge_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Pod status bridge unavailable: %s",
            esp_err_to_name(pod_status_bridge_err)
        );
    }

    app_shutdown_wait_and_run();
}
