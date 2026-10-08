#include "app_pod_status_bridge.h"
#include "app_pod_status_inventory_policy.h"
#include "app_shutdown.h"
#include "app_startup.h"
#include "board_config.h"
#include "device_status.h"
#include "hw_inventory.h"
#include "ina3221_monitor.h"
#include "nrf_emergency_stop_monitor.h"
#include "nrf_topology_service.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "APP";

#define APP_MAIN_TOPOLOGY_PROBE_WAIT_MS 2000U


static void app_main_report_topology_inventory(esp_err_t topology_start_err)
{
    const TickType_t started = xTaskGetTickCount();
    const TickType_t wait_ticks = pdMS_TO_TICKS(APP_MAIN_TOPOLOGY_PROBE_WAIT_MS);
    const TickType_t poll_ticks = pdMS_TO_TICKS(10U);
    bool topology_resolved = topology_start_err != ESP_OK;
    bool power_monitor_resolved = false;

    while (
        (TickType_t)(xTaskGetTickCount() - started) < wait_ticks &&
        (!topology_resolved || !power_monitor_resolved)
    ) {
        if (!topology_resolved) {
            nrf_topology_snapshot_t snapshot;
            if (nrf_topology_service_get_snapshot(&snapshot) == ESP_OK) {
                topology_resolved =
                    app_pod_status_bridge_report_topology_inventory(&snapshot);
            }
        }

        if (!power_monitor_resolved) {
            ina3221_monitor_rail_t rail;
            const esp_err_t rail_err =
                ina3221_monitor_get_rail(
                    INA3221_MONITOR_RAIL_BATTERY,
                    &rail);

            if (rail_err == ESP_OK && rail.available) {
                power_monitor_resolved = true;
                /* The monitor exposes invalid samples without distinguishing
                 * a missing INA3221 from a device fault to the main component. */
                hw_inventory_report(
                    HW_MODULE_POWER_MONITOR,
                    rail.valid ? HW_STATE_PRESENT : HW_STATE_ABSENT,
                    rail.valid ? "first sample ok" : "no valid sample");

                device_status_snapshot_t status;
                if (device_status_get_snapshot(&status) == ESP_OK) {
                    const device_main_status_t *main_status = &status.main;
                    const bool sensor_present =
                        main_status->bmp280_present || main_status->aht20_present;
                    const bool sensor_valid =
                        (main_status->bmp280_present && main_status->bmp280_valid) ||
                        (main_status->aht20_present && main_status->aht20_valid);

                    if (sensor_valid) {
                        hw_inventory_report(
                            HW_MODULE_BAROMETER,
                            HW_STATE_PRESENT,
                            "sensor ok");
                    } else if (sensor_present) {
                        hw_inventory_report(
                            HW_MODULE_BAROMETER,
                            HW_STATE_FAULT,
                            "sensor invalid");
                    } else {
                        hw_inventory_report(
                            HW_MODULE_BAROMETER,
                            HW_STATE_ABSENT,
                            "no response");
                    }
                }
            }
        }

        if (topology_resolved && power_monitor_resolved) {
            break;
        }

        const TickType_t elapsed =
            (TickType_t)(xTaskGetTickCount() - started);
        if (elapsed >= wait_ticks) {
            break;
        }

        const TickType_t remaining_ticks = wait_ticks - elapsed;
        vTaskDelay(poll_ticks < remaining_ticks ? poll_ticks : remaining_ticks);
    }

    hw_inventory_log_summary();

    hw_module_id_t first_missing = HW_MODULE_COUNT;
    if (hw_inventory_required_missing(&first_missing)) {
        ESP_LOGW(
            TAG,
            "BOOT_DEGRADED required module missing: %s",
            hw_inventory_module_name(first_missing));
    }
}


void app_main(void)
{
    ESP_ERROR_CHECK(board_gpio_isr_service_install());
    ESP_LOGI(TAG, "ISR service owner: board_config (IRAM)");

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

    app_main_report_topology_inventory(topology_err);

    app_shutdown_wait_and_run();
}
