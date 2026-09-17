#include "app_shutdown.h"

#include "app_poweroff.h"

#include "display.h"
#include "ina3221_monitor.h"
#include "insights_bridge.h"
#include "keypad.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_recovery.h"
#include "object_service.h"
#include "power_control.h"
#include "sd_card.h"
#include "seismic_acquisition.h"
#include "seismic_recorder.h"
#include "sensor_bus.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "runtime_observability.h"

#include "esp_err.h"
#include "esp_log.h"


static const char *TAG =
    "APP";


static void app_shutdown_stop_network(void)
{
    insights_bridge_shutdown();

    esp_err_t err =
        web_server_stop();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop web server: %s",
            esp_err_to_name(err)
        );
    }

    err =
        wifi_manager_stop();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop Wi-Fi: %s",
            esp_err_to_name(err)
        );
    }
}


static void app_shutdown_stop_storage(void)
{
    esp_err_t err =
        storage_manager_shutdown();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Storage shutdown failed: %s",
            esp_err_to_name(err)
        );
    }

    err =
        sd_card_unmount();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "SD card unmount failed: %s",
            esp_err_to_name(err)
        );
    } else {
        ESP_LOGI(
            TAG,
            "SD card unmounted"
        );
    }
}


static void app_shutdown_stop_display(void)
{
    esp_err_t err =
        display_set_enabled(
            false
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to switch display off: %s",
            esp_err_to_name(err)
        );
    }

    display_deinit();

    ESP_LOGI(
        TAG,
        "Display off"
    );
}


void app_shutdown_wait_and_run(void)
{
    power_control_wait_for_shutdown_request();

    ESP_LOGW(
        TAG,
        "Safe shutdown started"
    );

    keypad_stop_event_monitor();

    /*
     * Recovery completion can invalidate/refresh the Web object cache. Drain
     * that worker before Web lifecycle/cache teardown so no callback can land
     * after web_server_stop() has deleted its state. The worker releases the
     * storage guard between objects, so this waits only for the current bounded
     * recovery window.
     */
    object_recovery_stop_background();

    /*
     * Stop command ingress while measurement, recorder, object and storage
     * dependencies are still alive. web_server_stop() is the synchronous HTTP
     * drain barrier; Wi-Fi is stopped only after those handlers have exited.
     * Errors remain fail-safe: app_shutdown_stop_network() logs them and the
     * physical power-off sequence continues.
     */
    app_shutdown_stop_network();

    /*
     * The topology worker owns the multi-step TCA select/use/close sequence
     * and its 0x30/0x31/0x32 device handles. Stop it before the shared I2C0
     * owner is torn down so shutdown cannot delete a bus below a live worker.
     */
    nrf_topology_service_stop();

    ina3221_monitor_stop();

    const esp_err_t sensor_bus_err =
        sensor_bus_deinit();
    if (sensor_bus_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Sensor I2C shutdown failed: %s",
            esp_err_to_name(sensor_bus_err)
        );
    }

    /*
     * Stop measurement commands first, then finalize the recorder while the
     * LVGL display guard is still alive. The recorder and TFT share SPI2 with
     * the SD card; tearing LVGL down first can otherwise leave shutdown waiting
     * on the same display/storage arbitration that an active recording needs.
     */
    measurement_service_shutdown();

    esp_err_t recorder_err =
        seismic_recorder_stop();

    if (recorder_err == ESP_OK) {
        recorder_err = seismic_recorder_wait_for_stop();
    }

    if (
        recorder_err != ESP_OK &&
        recorder_err != ESP_ERR_INVALID_STATE
    ) {
        ESP_LOGW(
            TAG,
            "Recorder shutdown failed: %s",
            esp_err_to_name(recorder_err)
        );
    }

    runtime_observability_log_snapshot("after-recorder-finalization");

    /* No recorder/recovery SD traffic remains after this point, so LVGL can exit. */
    ui_shutdown();

    storage_service_shutdown();

    object_service_shutdown();

    /*
     * Acquisition remains alive until the recording stop cursor has been
     * drained above. Only now is it safe to reset the seismic buffer.
     */
    seismic_acquisition_stop();

    app_shutdown_stop_storage();

    app_shutdown_stop_display();

    runtime_observability_stop();

    ESP_LOGI(
        TAG,
        "Cutting power"
    );

    power_control_shutdown();

    app_poweroff_idle_forever();
}
