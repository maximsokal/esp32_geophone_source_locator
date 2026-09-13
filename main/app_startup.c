#include "app_startup.h"

#include "app_startup_status.h"
#include "app_storage_boot.h"

#include <stddef.h>
#include <stdint.h>

#include "adc_service.h"
#include "app_events.h"
#include "device_status.h"
#include "display.h"
#include "ina3221_monitor.h"
#include "insights_bridge.h"
#include "keypad.h"
#include "measurement_service.h"
#include "object_recovery.h"
#include "object_service.h"
#include "object_store.h"
#include "power_control.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "spi_bus.h"
#include "status_indicator.h"
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


static void app_startup_log_elapsed(
    const char *stage,
    uint32_t started_ms
)
{
    const uint32_t elapsed_ms =
        esp_log_timestamp() -
        started_ms;

    ESP_LOGI(
        TAG,
        "BOOT_TIMING: %s=%u ms",
        stage,
        (unsigned)elapsed_ms
    );
}


static void app_startup_background_recovery_complete(
    esp_err_t recovery_result,
    void *user_context
)
{
    (void)user_context;

    if (recovery_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Background object recovery completed with error before Web cache refresh: %s",
            esp_err_to_name(recovery_result)
        );
    }

    const esp_err_t refresh_err =
        web_server_enable_status_cache_refresh();
    if (refresh_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Deferred Web status cache refresh request failed: %s",
            esp_err_to_name(refresh_err)
        );
    }
}


static void app_startup_initialize_core(void)
{
    ESP_ERROR_CHECK(
        app_events_init()
    );

    ESP_ERROR_CHECK(
        power_control_init()
    );

    ESP_ERROR_CHECK(
        device_status_init()
    );

    const esp_err_t status_indicator_err =
        status_indicator_init();
    if (status_indicator_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Status indicator subscriber unavailable: %s",
            esp_err_to_name(status_indicator_err)
        );
    }

    /*
     * INA3221 is useful telemetry, not a boot-critical dependency. The
     * monitor task keeps retrying when the board is absent or I2C is broken,
     * so hardware bring-up cannot trap the controller in a reboot loop.
     */
    const esp_err_t power_monitor_err =
        ina3221_monitor_start();

    if (power_monitor_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "INA3221 monitor unavailable: %s",
            esp_err_to_name(
                power_monitor_err
            )
        );
    }

    ESP_ERROR_CHECK(
        seismic_buffer_init()
    );

    ESP_ERROR_CHECK(
        adc_service_init()
    );

    ESP_ERROR_CHECK(
        keypad_init()
    );

    const uint32_t spi_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        app_spi_bus_init()
    );

    app_startup_log_elapsed(
        "spi_bus_init",
        spi_started_ms
    );
}


static void app_startup_initialize_services(void)
{
    uint32_t stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        display_init()
    );

    app_startup_log_elapsed(
        "display_init",
        stage_started_ms
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        ui_boot_init()
    );

    runtime_observability_log_snapshot("after-lvgl");

    app_startup_log_elapsed(
        "boot_screen",
        stage_started_ms
    );

    ui_boot_set_stage(
        "STORAGE..."
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        storage_manager_init()
    );

    app_startup_log_elapsed(
        "storage_manager_init",
        stage_started_ms
    );

    /*
     * Only the active object is recovery-critical for interactive startup.
     * Repair it before object_store_init() restores active state, but do not
     * walk every historical object here: a large archive used to keep the
     * boot screen and keypad gate blocked for tens of seconds.
     */
    ui_boot_set_stage(
        "ACTIVE OBJECT..."
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        object_recovery_run_active()
    );

    app_startup_log_elapsed(
        "object_recovery_active",
        stage_started_ms
    );

    ui_boot_set_stage(
        "OBJECT STORE..."
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        object_store_init()
    );

    app_startup_log_elapsed(
        "object_store_init",
        stage_started_ms
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        seismic_recorder_init()
    );

    app_startup_log_elapsed(
        "seismic_recorder_init",
        stage_started_ms
    );

    ui_boot_set_stage(
        "NETWORK + UI..."
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        object_service_init()
    );

    ESP_ERROR_CHECK(
        measurement_service_init()
    );

    ESP_ERROR_CHECK(
        storage_service_init()
    );

    runtime_observability_log_snapshot("after-service-tasks");

    ESP_ERROR_CHECK(
        wifi_manager_init()
    );

    const esp_err_t insights_err =
        insights_bridge_init();
    if (insights_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP Insights bridge unavailable: %s",
            esp_err_to_name(insights_err));
    }

    ESP_ERROR_CHECK(
        ui_init()
    );

    ESP_ERROR_CHECK(
        web_server_start()
    );

    ESP_ERROR_CHECK(
        wifi_manager_start()
    );

    runtime_observability_log_snapshot("after-wifi");

    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );

    app_startup_log_elapsed(
        "runtime_services",
        stage_started_ms
    );

    /*
     * Acquisition hardware is allowed to be absent/faulty during
     * bring-up. The LVGL UI remains usable and reports STOPPED/error
     * state instead of rebooting the whole controller.
     */
    stage_started_ms =
        esp_log_timestamp();

    const esp_err_t acquisition_err =
        seismic_acquisition_start();

    app_startup_log_elapsed(
        "seismic_acquisition_start",
        stage_started_ms
    );

    if (acquisition_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Seismic acquisition unavailable: %s",
            esp_err_to_name(
                acquisition_err
            )
        );
    }
}


void app_startup_run(void)
{
    const uint32_t startup_started_ms =
        esp_log_timestamp();

    if (!runtime_observability_start()) {
        ESP_LOGW(TAG, "Runtime observability unavailable");
    }

    app_startup_initialize_core();

    const uint32_t sd_started_ms =
        esp_log_timestamp();

    app_storage_boot_run_diagnostic();

    app_startup_log_elapsed(
        "sd_boot_diagnostic",
        sd_started_ms
    );

    /*
     * Prepare/seed the immutable React bundle immediately after the SD mount,
     * before display_init() creates the esp_lcd device on shared SPI2. This
     * guarantees that Web UI seed FAT/SDSPI traffic cannot overlap TFT DMA.
     * Runtime HTTP delivery itself is flash-only.
     */
    const uint32_t webui_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        web_server_prepare_webui()
    );

    app_startup_log_elapsed(
        "webui_prepare",
        webui_started_ms
    );

    /* web_server_start() creates the asynchronous object/session status cache
     * and historically queued a 32-object FATFS walk here. Close its refresh
     * gate before service startup so startup_total measures only the
     * interactive path. Recovery completion will reopen and explicitly queue
     * the first archive-wide cache refresh. */
    web_server_defer_status_cache_refresh();

    app_startup_initialize_services();

    /*
     * GPIO21 must not be able to latch shutdown while storage preparation and
     * service startup are still running. Arm after the interactive UI, keypad
     * delivery and acquisition startup have all completed. Full historical
     * object recovery is background work and is safe to cancel during shutdown.
     */
    const uint32_t power_arm_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        power_control_arm_shutdown_button()
    );

    app_startup_log_elapsed(
        "power_button_arm",
        power_arm_started_ms
    );

    app_startup_log_success();

    (void)insights_bridge_publish_runtime();

    runtime_observability_log_snapshot("boot");

    app_startup_log_elapsed(
        "startup_total",
        startup_started_ms
    );

    /*
     * Start the expensive archive-wide recovery only after startup_total has
     * been committed to the log and the UI/keypad are already interactive.
     * The first Web object/session cache refresh remains gated until this
     * worker completes, preventing two archive-wide SD scans from alternating
     * TFT quiesce windows and making keypad events stale.
     */
    const esp_err_t recovery_err =
        object_recovery_start_background_with_callback(
            app_startup_background_recovery_complete,
            NULL
        );
    if (recovery_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Background object recovery unavailable: %s",
            esp_err_to_name(recovery_err)
        );

        /* Do not leave the Web object/session cache permanently disabled when
         * the optional recovery worker cannot be created. */
        app_startup_background_recovery_complete(
            recovery_err,
            NULL
        );
    }
}
