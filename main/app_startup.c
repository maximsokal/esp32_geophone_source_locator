#include "app_startup.h"

#include "app_startup_status.h"
#include "app_storage_boot.h"

#include <stddef.h>
#include <stdint.h>

#include "adc_service.h"
#include "app_event_bus.h"
#include "device_status.h"
#include "display.h"
#include "keypad.h"
#include "object_recovery.h"
#include "object_service.h"
#include "object_store.h"
#include "power_control.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "spi_bus.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

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


static void app_startup_initialize_core(void)
{
    ESP_ERROR_CHECK(
        app_event_bus_init()
    );

    ESP_ERROR_CHECK(
        power_control_init()
    );

    ESP_ERROR_CHECK(
        device_status_init()
    );

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

    ui_boot_set_stage(
        "RECOVERY..."
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        object_recovery_run()
    );

    app_startup_log_elapsed(
        "object_recovery_run",
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

    ui_boot_set_stage(
        "NETWORK + UI..."
    );

    stage_started_ms =
        esp_log_timestamp();

    ESP_ERROR_CHECK(
        object_service_init()
    );

    ESP_ERROR_CHECK(
        storage_service_init()
    );

    ESP_ERROR_CHECK(
        wifi_manager_init()
    );

    ESP_ERROR_CHECK(
        wifi_manager_start()
    );

    ESP_ERROR_CHECK(
        web_server_start()
    );

    ESP_ERROR_CHECK(
        ui_init()
    );

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

    app_startup_initialize_core();

    const uint32_t sd_started_ms =
        esp_log_timestamp();

    app_storage_boot_run_diagnostic();

    app_startup_log_elapsed(
        "sd_boot_diagnostic",
        sd_started_ms
    );

    app_startup_initialize_services();

    app_startup_log_success();

    app_startup_log_elapsed(
        "startup_total",
        startup_started_ms
    );
}
