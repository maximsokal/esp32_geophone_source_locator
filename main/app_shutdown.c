#include "app_shutdown.h"
#include "app_shutdown_drain.h"
#include "app_stop_timeout_config.h"
#include "app_keypad_config.h"

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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


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
#if APP_DISPLAY_ENABLED
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
#endif
}


/* Poll period of the write drain; short enough for the TFT and the Web UI to follow the stop. */
#define APP_SHUTDOWN_DRAIN_POLL_MS 100U


static uint64_t app_shutdown_drain_now_ms(void *context)
{
    (void)context;
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}


static void app_shutdown_drain_sleep_ms(void *context, uint32_t ms)
{
    (void)context;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0U) ticks = 1U;
    vTaskDelay(ticks);
}


/* Anything that still has data on its way to the SD card, or is about to produce some. */
static bool app_shutdown_drain_writes_pending(void *context)
{
    (void)context;
    measurement_service_snapshot_t measurement;
    if (measurement_service_get_snapshot(&measurement) == ESP_OK) {
        if (measurement.busy ||
            measurement.state == MEASUREMENT_SERVICE_STARTING ||
            measurement.state == MEASUREMENT_SERVICE_RECORDING ||
            measurement.state == MEASUREMENT_SERVICE_STOPPING ||
            measurement.state == MEASUREMENT_SERVICE_DELETING) {
            return true;
        }
    }
    seismic_recorder_snapshot_t recorder;
    if (seismic_recorder_get_snapshot(&recorder) == ESP_OK &&
        (recorder.state == SEISMIC_RECORDER_RECORDING || recorder.state == SEISMIC_RECORDER_STOPPING)) {
        return true;
    }
    return object_service_is_busy() || storage_service_is_busy();
}


static void app_shutdown_drain_request_stop(void *context)
{
    (void)context;
    const esp_err_t err = measurement_service_stop_async();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Could not ask the recording to stop: %s", esp_err_to_name(err));
    }
}


static void app_shutdown_drain_progress(void *context, uint32_t waited_ms)
{
    (void)context;
    if (waited_ms / 1000U != (waited_ms - APP_SHUTDOWN_DRAIN_POLL_MS) / 1000U) {
        ESP_LOGW(TAG, "Writing out queued data before power off (%u s)", (unsigned)(waited_ms / 1000U));
    }
}


void app_shutdown_wait_and_run(void)
{
    power_control_wait_for_shutdown_request();

    ESP_LOGW(
        TAG,
        "Safe shutdown started (reason: %s)",
        power_control_shutdown_reason_name(
            power_control_shutdown_reason()
        )
    );

    /*
     * A START that is waiting for a Channel Sounding cycle holds the measurement worker, and so
     * the whole shutdown, for the rest of that cycle (up to ~90 s with five nodes). Stop the
     * cycle, release the nodes and refuse new cycles now; the START then fails at once.
     */
    (void)nrf_topology_service_ranging_disable();

#if APP_KEYPAD_ENABLED
    keypad_stop_event_monitor();
#endif

    /* Free CPU1 before draining HTTP; keep the final samples for the recorder. */
    seismic_acquisition_prepare_shutdown();

    /*
     * Recovery completion can invalidate/refresh the Web object cache. Drain
     * that worker before Web lifecycle/cache teardown so no callback can land
     * after web_server_stop() has deleted its state. The worker releases the
     * storage guard between objects, so this waits only for the current bounded
     * recovery window.
     */
    object_recovery_stop_background();

    /*
     * Battery critical or the button: write out everything already on its way to the SD card and
     * take nothing new (the shutdown latch makes the Web UI refuse new commands and the keypad is
     * stopped). The network stays up for this wait, so the Web UI and the TFT keep showing
     * "critical battery - stopping" until the queue is gone. The wait is bounded: the device
     * switches off afterwards whatever the outcome.
     */
    {
        const app_shutdown_drain_ops_t drain_ops = {
            .now_ms = app_shutdown_drain_now_ms,
            .sleep_ms = app_shutdown_drain_sleep_ms,
            .writes_pending = app_shutdown_drain_writes_pending,
            .request_stop = app_shutdown_drain_request_stop,
            .progress = app_shutdown_drain_progress,
        };
        app_shutdown_drain_report_t drain_report;
        const app_shutdown_drain_result_t drain_result = app_shutdown_drain_writes(
            &drain_ops,
            APP_STOP_TIMEOUT_RECORDING_MS,
            APP_SHUTDOWN_DRAIN_POLL_MS,
            &drain_report
        );
        ESP_LOGW(
            TAG,
            "Write drain %s after %u ms (stop requested: %s)",
            app_shutdown_drain_result_name(drain_result),
            (unsigned)drain_report.waited_ms,
            drain_report.stop_requested ? "yes" : "no"
        );
    }

    /*
     * Stop command ingress while measurement, recorder, object and storage
     * dependencies are still alive. web_server_stop() is the synchronous HTTP
     * drain barrier; Wi-Fi is stopped only after those handlers have exited.
     * Errors remain fail-safe: app_shutdown_stop_network() logs them and the
     * physical power-off sequence continues.
     */
    app_shutdown_stop_network();

    runtime_observability_set_health_phase(
        RUNTIME_TASK_CONTROL,
        RUNTIME_HEALTH_PHASE_WAITING
    );
    runtime_observability_beat(RUNTIME_TASK_CONTROL, 0U);
    const esp_err_t measurement_err = measurement_service_shutdown();
    const app_shutdown_plan_t plan =
        app_shutdown_plan_after_measurement(measurement_err == ESP_OK);
    if (!plan.full_teardown) {
        /*
         * The measurement worker did not stop in time. The battery is critical, so the power is
         * not kept on for it: the services that worker may still be using are left alone (no
         * teardown below its feet) and only the power is cut. The queued data was written out by
         * the drain above, which already had the full recording-stop budget.
         */
        ESP_LOGE(
            TAG,
            "Measurement service shutdown failed: %s; cutting power without tearing its dependencies down",
            esp_err_to_name(measurement_err)
        );
        runtime_observability_log_snapshot("measurement-shutdown-failed");
        power_control_shutdown();
        app_poweroff_idle_forever();
    }
    runtime_observability_set_health_phase(
        RUNTIME_TASK_CONTROL,
        RUNTIME_HEALTH_PHASE_IDLE
    );
    runtime_observability_beat(RUNTIME_TASK_CONTROL, 1U);

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

    /* No recorder/recovery SD traffic remains after this point, so LVGL can exit when enabled. */
#if APP_DISPLAY_ENABLED
    ui_shutdown();
#endif

    storage_service_shutdown();

    object_service_shutdown();

    /*
     * The producer was quiesced before the HTTP barrier. Only now is it safe
     * to reset the buffer retained for the recording stop cursor.
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
