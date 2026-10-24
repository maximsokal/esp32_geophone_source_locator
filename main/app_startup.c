#include "app_startup.h"

#include "app_startup_status.h"
#include "app_startup_module_policy.h"
#include "app_storage_boot.h"
#include "app_storage_boot_ui.h"

#include <stddef.h>
#include <stdint.h>

#include "adc_service.h"
#include "app_events.h"
#include "device_status.h"
#include "display.h"
#include "ina3221_monitor.h"
#include "insights_bridge.h"
#include "hw_inventory.h"
#include "keypad.h"
#include "localization_job.h"
#include "measurement_service.h"
#include "object_recovery.h"
#include "object_service.h"
#include "workflow_service.h"
#include "object_store.h"
#include "power_control.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "sensor_bus.h"
#include "spi_bus.h"
#include "status_indicator.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "runtime_observability.h"
#include "app_keypad_config.h"

#if CONFIG_APP_TEST_WDT_TRIGGER
#include "app_test_wdt.h"
#endif

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_STARTUP_STACK_HWM_TARGET_BYTES 1024U

static const char *TAG =
    "APP";


static app_startup_module_policy_t app_startup_snapshot_module_policy(void)
{
    hw_module_info_t modules[HW_MODULE_COUNT];
    const size_t count = hw_inventory_snapshot(modules, HW_MODULE_COUNT);
    return app_startup_module_policy_from_inventory(modules, count);
}


/* MAX-183: runs on the INA3221 monitor task. */
static void app_startup_request_low_battery_shutdown(void)
{
    const esp_err_t err =
        power_control_request_shutdown(POWER_SHUTDOWN_REASON_LOW_BATTERY);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Low-battery shutdown request failed: %s",
            esp_err_to_name(err)
        );
    }
}


static void app_startup_log_stack_hwm(void)
{
    const UBaseType_t free_bytes =
        uxTaskGetStackHighWaterMark(NULL);

    if (free_bytes < APP_STARTUP_STACK_HWM_TARGET_BYTES) {
        ESP_LOGW(
            TAG,
            "STACK_HWM task=main free=%u target=%u",
            (unsigned)free_bytes,
            (unsigned)APP_STARTUP_STACK_HWM_TARGET_BYTES
        );
    } else {
        ESP_LOGI(
            TAG,
            "STACK_HWM task=main free=%u target=%u",
            (unsigned)free_bytes,
            (unsigned)APP_STARTUP_STACK_HWM_TARGET_BYTES
        );
    }
}


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

    /* Recovery can remove or repair historical objects outside object_store's
     * mutation API. Drop its RAM projection before reopening the already
     * deferred Web refresh so that refresh becomes the one authoritative
     * rebuild instead of publishing stale profile entries for up to 60 s. */
    const esp_err_t cache_err =
        object_store_invalidate_profile_cache();
    if (cache_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Object profile cache invalidation after recovery failed: %s",
            esp_err_to_name(cache_err)
        );
    }

    const esp_err_t status_event_err =
        app_events_post_best_effort(
            APP_EVENT_STORAGE_RECOVERY_FINISHED,
            (int32_t)recovery_result
        );
    if (status_event_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Background recovery UI completion event dropped: %s",
            esp_err_to_name(status_event_err)
        );
    }

    if (recovery_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Background object/session recovery completed with error before Web cache refresh: %s",
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


static void app_startup_recorder_recovery_requested(
    const char *object_id,
    const char *session_id,
    void *user_context
)
{
    (void)user_context;

    ESP_LOGW(
        TAG,
        "Recorder terminal metadata commit failed; queueing session recovery object=%s session=%s",
        object_id != NULL ? object_id : "?",
        session_id != NULL ? session_id : "?"
    );

    /* Keep a cache refresh from publishing the stale `recording` placeholder
     * while recovery is pending. The completion callback is the single point
     * that reopens the refresh gate and queues a fresh archive snapshot. */
    web_server_defer_status_cache_refresh();

    (void)app_events_post_best_effort(
        APP_EVENT_STORAGE_RECOVERY_STARTED,
        0
    );

    const esp_err_t recovery_err =
        object_recovery_start_background_with_callback(
            app_startup_background_recovery_complete,
            NULL
        );
    if (recovery_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Recorder recovery request could not start background worker: %s",
            esp_err_to_name(recovery_err)
        );
        app_startup_background_recovery_complete(
            recovery_err,
            NULL
        );
        return;
    }

    /* If the request arrived while a previous worker was already executing
     * its completion callback, start_background waits for that bounded teardown
     * and creates a successor. The old callback may have reopened the cache in
     * the meantime, so close the gate once more after successful handoff. For
     * normal coalesced rescans this is simply an idempotent second defer. */
    web_server_defer_status_cache_refresh();
}


static void app_startup_initialize_core(void)
{
    /*
     * Create sensor_bus's state mutex before any task exists that could
     * call into sensor_bus_init()/get_handles()/get_tca9548a()/deinit()
     * concurrently (the INA3221 monitor task, later the nRF topology
     * service task). This function runs synchronously on the single boot
     * task, so there is no bootstrap race here even though the lock is
     * lazily created (nothing else can be racing it yet).
     */
    ESP_ERROR_CHECK(
        sensor_bus_lock_init()
    );

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

    ESP_ERROR_CHECK(
        seismic_buffer_init()
    );

    ESP_ERROR_CHECK(
        seismic_acquisition_init()
    );

    /*
     * RESET/CONVST are intentionally left floating (not driven) from boot
     * until power permits a bounded preflight or an explicit capture start. Driving
     * them low unconditionally was tried and reverted: the AD7606 is
     * physically powered independent of the driver, and a measured VIO
     * back-feed path exists when its own 5V is disconnected -- actively
     * sinking current into that path from RESET/CONVST is a real hazard,
     * not a safe idle state, until AD7606 presence/power can be confirmed
     * first. Do not reintroduce this without gating it on that.
     */
#if APP_KEYPAD_ENABLED
    ESP_ERROR_CHECK(
        adc_service_init()
    );

    ESP_ERROR_CHECK(
        keypad_init()
    );
#endif

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


static void app_startup_initialize_services(
    uint32_t startup_started_ms,
    const app_startup_module_policy_t *module_policy
)
{
    uint32_t stage_started_ms =
        esp_log_timestamp();

#if APP_DISPLAY_ENABLED
    ESP_ERROR_CHECK(
        app_storage_boot_ui_ensure_display_initialized()
    );
#endif

    app_startup_log_elapsed(
        "display_init",
        stage_started_ms
    );

    stage_started_ms =
        esp_log_timestamp();

#if APP_DISPLAY_ENABLED
    ESP_ERROR_CHECK(
        app_storage_boot_ui_ensure_initialized()
    );

    runtime_observability_log_snapshot("after-lvgl");
#endif

    app_startup_log_elapsed(
        "boot_screen",
        stage_started_ms
    );

#if APP_DISPLAY_ENABLED
    ui_boot_set_stage(
        module_policy->storage_allowed
            ? "STORAGE..."
            : "STORAGE UNAVAILABLE"
    );
#endif

    stage_started_ms =
        esp_log_timestamp();

    if (module_policy->storage_allowed) {
        esp_err_t storage_err = storage_manager_init();
        if (storage_err != ESP_OK) {
            app_storage_boot_recover_storage_manager_failure(storage_err);
        }
    } else {
        ESP_LOGW(TAG, "SD card is not PRESENT; skipping storage manager initialization");
    }

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
    if (module_policy->storage_allowed) {
#if APP_DISPLAY_ENABLED
        ui_boot_set_stage(
            "ACTIVE OBJECT..."
        );
#endif

        stage_started_ms =
            esp_log_timestamp();

        ESP_ERROR_CHECK(
            object_recovery_run_active()
        );

        app_startup_log_elapsed(
            "object_recovery_active",
            stage_started_ms
        );

#if APP_DISPLAY_ENABLED
        ui_boot_set_stage(
            "OBJECT STORE..."
        );
#endif

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
        ESP_ERROR_CHECK(
            seismic_recorder_set_recovery_request_callback(
                app_startup_recorder_recovery_requested,
                NULL
            )
        );

        app_startup_log_elapsed(
            "seismic_recorder_init",
            stage_started_ms
        );
    } else {
        ESP_LOGW(TAG, "Skipping SD-backed active-object recovery, object store, and recorder startup");
    }

#if APP_DISPLAY_ENABLED
    ui_boot_set_stage(
        "NETWORK + UI..."
    );
#endif

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        object_service_init()
    );

    ESP_ERROR_CHECK(
        measurement_service_init()
    );

    ESP_ERROR_CHECK(
        workflow_service_init()
    );

    /* "Calculate source" runs on its own worker (never on the HTTPS task);
     * it must exist before the web server accepts requests. */
    ESP_ERROR_CHECK(
        localization_job_init()
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

    /* Probe after power telemetry starts below; continuous acquisition still
     * requires a live preview or recording request. */

    ESP_ERROR_CHECK(
        wifi_manager_start()
    );

    runtime_observability_log_snapshot("after-wifi");

    ESP_ERROR_CHECK(
        web_server_start()
    );

    /*
     * Tried and reverted (2026-09-20): running wifi_manager_init()/start()
     * first, before display_init()/ui_boot_init()/storage/service tasks, on
     * the theory that Core 0 being near-saturated (LVGL + three service
     * tasks, ~94-100% busy per RUNTIME_CPU snapshots) by the time
     * esp_wifi_start()/phy_init() ran was extending or worsening the current
     * draw window. Three back-to-back captures with that order still hit the
     * identical brownout at the identical `phy_init: phy_version ...` line,
     * with the identical rst:0x3/rtc_brownout_isr_handler signature, now
     * with zero Core 0 contention (display_init() never even ran, since
     * Wi-Fi always crashed first) -- which rules out CPU/task-scheduling
     * contention as the limiting factor. Do not retry this reorder without
     * new evidence; the boot order below is not the cause.
     */

    /*
     * Keep the dynamic LVGL timer and UI event ingress out of
     * esp_wifi_start()/phy_init(). With the display enabled, the boot screen
     * is already visible while ui_init() starts the 42 ms render timer and
     * allows TFT DMA activity.
     * Starting that runtime work before the radio transition creates an
     * avoidable shared-rail load peak exactly at the brownout boundary.
     * web_server_start() is also deliberately after Wi-Fi: it creates the
     * status-cache and lifecycle tasks, while its relevant IP/AP handlers are
     * registered before those asynchronous transitions complete and it
     * performs an immediate state check for an already-running interface.
     */
#if APP_DISPLAY_ENABLED
    ESP_ERROR_CHECK(
        ui_init()
    );
#endif

#if APP_KEYPAD_ENABLED
    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );
#endif

    app_startup_log_elapsed(
        "interactive_ready",
        startup_started_ms
    );

    /*
     * INA3221 is telemetry, not a prerequisite for Wi-Fi. Do not let its I2C
     * worker open the shared sensor bus or perform conversions while
     * esp_wifi_start() is bringing up the PHY: that creates an avoidable
     * startup load peak exactly at the brownout boundary. The monitor remains
     * non-fatal and starts immediately after the radio transition completes.
     */
    const uint32_t power_monitor_started_ms =
        esp_log_timestamp();

    /* MAX-183: a confirmed critical battery that outlasts the grace period
     * starts the same safe shutdown as the power button. */
    ina3221_monitor_set_critical_battery_handler(
        app_startup_request_low_battery_shutdown
    );

    ESP_LOGI(TAG, "BOOT_PHASE wifi_start=complete i2c_start=allowed");
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

    /* Power telemetry may arrive later; the measurement worker retries a
     * power-deferred probe without starting the continuous source. */
    const esp_err_t probe_err = seismic_acquisition_probe();
    if (probe_err != ESP_OK && probe_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Seismic preflight unavailable: %s", esp_err_to_name(probe_err));
    }

    app_startup_log_elapsed(
        "sensor_bus_i2c_start_after_wifi",
        power_monitor_started_ms
    );

    app_startup_log_elapsed(
        "runtime_services",
        stage_started_ms
    );
}


void app_startup_run(void)
{
    hw_inventory_init();

    const uint32_t startup_started_ms =
        esp_log_timestamp();

    if (!runtime_observability_start()) {
        ESP_LOGW(TAG, "Runtime observability unavailable");
    }

    app_startup_initialize_core();

    app_storage_boot_ui_register();

    const uint32_t sd_started_ms =
        esp_log_timestamp();

    app_storage_boot_run_diagnostic();

    app_startup_log_elapsed(
        "sd_boot_diagnostic",
        sd_started_ms
    );

    const app_startup_module_policy_t module_policy =
        app_startup_snapshot_module_policy();
    if (!module_policy.storage_allowed) {
        ESP_LOGW(TAG, "Storage-dependent boot services will be skipped; UI and network startup continue");
    }

    /*
     * On the healthy-card path, prepare/seed the immutable React bundle
     * before the shared boot display is initialized. A recovery prompt may
     * initialize the display earlier so the user can see the failure and
     * format progress. Runtime HTTP delivery itself is flash-only.
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
     * gate before service startup so neither interactive_ready nor
     * startup_total includes an archive-wide cache scan. Recovery completion
     * will reopen and explicitly queue the first archive-wide cache refresh. */
    if (module_policy.storage_allowed) {
        web_server_defer_status_cache_refresh();
    }

    app_startup_initialize_services(
        startup_started_ms,
        &module_policy
    );

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

#if CONFIG_APP_TEST_WDT_TRIGGER
    app_test_wdt_start();
#endif

    (void)insights_bridge_publish_runtime();

    runtime_observability_log_snapshot("boot");

    app_startup_log_elapsed(
        "startup_total",
        startup_started_ms
    );

    /*
     * Explicit deterministic HWM sample for the task running app_main(),
     * taken right after the complete synchronous startup path instead of
     * depending on whether "main" happens to appear in the periodic
     * runtime-observability top-N sample.
     */
    app_startup_log_stack_hwm();

    /*
     * Start the expensive archive-wide recovery only after startup_total has
     * been committed to the log and the UI/keypad have already been
     * interactive since interactive_ready. The first Web object/session cache
     * refresh remains gated until this worker completes, preventing two
     * archive-wide SD scans from alternating TFT quiesce windows and making
     * keypad events stale.
     */
    if (module_policy.storage_allowed) {
        const esp_err_t recovery_status_err =
            app_events_post_best_effort(
                APP_EVENT_STORAGE_RECOVERY_STARTED,
                0
            );
        if (recovery_status_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Background recovery UI start event dropped: %s",
                esp_err_to_name(recovery_status_err)
            );
        }

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
    } else {
        const esp_err_t refresh_err = web_server_enable_status_cache_refresh();
        if (refresh_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Web status cache refresh request failed in degraded storage mode: %s",
                esp_err_to_name(refresh_err)
            );
        }
    }
}
