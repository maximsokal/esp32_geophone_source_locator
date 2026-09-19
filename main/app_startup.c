#include "app_startup.h"

#include "app_startup_status.h"
#include "app_storage_boot.h"

#include <stddef.h>
#include <stdint.h>

#include "ad7606_driver.h"
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
#include "sensor_bus.h"
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
     * The AD7606 is physically powered (AVCC/VIO) from board power-on,
     * independent of when/whether its driver is fully brought up. Without
     * this, RESET and CONVST are left floating from boot until the user
     * opens Measurements or issues a Web start, which can be minutes or
     * never. This only configures GPIO idle levels (CS high, RESET/CONVST
     * low, BUSY input); it does not touch SPI3, reset-pulse, or self-test,
     * so it is safe even when no AD7606 is physically connected.
     */
    const esp_err_t ad7606_safe_gpio_err =
        ad7606_driver_safe_gpio_init();

    if (ad7606_safe_gpio_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "AD7606 safe GPIO init failed: %s",
            esp_err_to_name(ad7606_safe_gpio_err)
        );
    }

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


static void app_startup_initialize_services(
    uint32_t startup_started_ms
)
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
     * The shared I2C0 sensor bus (INA3221, and later the TCA9548A/nRF54L15
     * upstream topology) is only opened after the keypad, display and SD
     * card stack are already interactive. A main-enclosure XIAO nRF54L15 is
     * wired as an upstream I2C target on this same bus; probing it before
     * the rest of local hardware bring-up is complete previously risked
     * stalling boot on a bus that a still-booting nRF node could be holding
     * in an indeterminate state. INA3221 remains non-boot-critical: the
     * monitor task keeps retrying when the board is absent or I2C is
     * broken, so this ordering only changes when the retry loop starts.
     */
    stage_started_ms =
        esp_log_timestamp();

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

    app_startup_log_elapsed(
        "sensor_bus_i2c_start",
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

    /*
     * Local TFT navigation is the interactive boot boundary. Arm keypad
     * delivery immediately after ui_init() has installed the dispatcher;
     * acquisition startup may run after this point without delaying local
     * controls, but Web/Wi-Fi is not exposed until that first transition ends.
     */
    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );

    app_startup_log_elapsed(
        "interactive_ready",
        startup_started_ms
    );

    /*
     * The AD7606 no longer auto-starts sampling at boot. Only its GPIOs
     * were put into a safe idle state by ad7606_driver_safe_gpio_init() in
     * app_startup_initialize_core() (see that call site); SPI3, the
     * hardware reset pulse and self-test have not run yet. Active
     * acquisition (which performs all of that via ad7606_driver_init())
     * now starts only when the local UI opens the Measurements screen
     * (ui_screen_registry.c) or a
     * Web client explicitly requests it (web_api_control.c), so the ADC is
     * not running with nobody consuming its samples. That start call is
     * idempotent, so both triggers may fire in either order.
     */

    ESP_ERROR_CHECK(
        web_server_start()
    );

    ESP_ERROR_CHECK(
        wifi_manager_start()
    );

    runtime_observability_log_snapshot("after-wifi");

    app_startup_log_elapsed(
        "runtime_services",
        stage_started_ms
    );
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
     * gate before service startup so neither interactive_ready nor
     * startup_total includes an archive-wide cache scan. Recovery completion
     * will reopen and explicitly queue the first archive-wide cache refresh. */
    web_server_defer_status_cache_refresh();

    app_startup_initialize_services(
        startup_started_ms
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

    (void)insights_bridge_publish_runtime();

    runtime_observability_log_snapshot("boot");

    app_startup_log_elapsed(
        "startup_total",
        startup_started_ms
    );

    /*
     * Start the expensive archive-wide recovery only after startup_total has
     * been committed to the log and the UI/keypad have already been
     * interactive since interactive_ready. The first Web object/session cache
     * refresh remains gated until this worker completes, preventing two
     * archive-wide SD scans from alternating TFT quiesce windows and making
     * keypad events stale.
     */
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
}
