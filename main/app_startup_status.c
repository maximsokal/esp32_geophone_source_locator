#include "app_startup_status.h"
#include "app_keypad_config.h"

#include <stdio.h>

#include "hw_inventory.h"
#include "object_recovery.h"
#include "seismic_acquisition.h"
#include "wifi_manager.h"
#include "web_server.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"


static const char *TAG =
    "APP";

static app_startup_reset_diagnostics_t s_reset_diagnostics;


const app_startup_reset_diagnostics_t *app_startup_get_reset_diagnostics(void)
{
    return s_reset_diagnostics.valid ? &s_reset_diagnostics : NULL;
}


static void app_startup_log_reset_reason(void)
{
    s_reset_diagnostics.reason = esp_reset_reason();
    (void)snprintf(
        s_reset_diagnostics.reason_name,
        sizeof(s_reset_diagnostics.reason_name),
        "%s",
        app_startup_reset_reason_name(s_reset_diagnostics.reason)
    );
    s_reset_diagnostics.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    s_reset_diagnostics.boot_id = object_recovery_boot_id();
    s_reset_diagnostics.valid = true;

    web_status_json_set_reset_diagnostics(
        s_reset_diagnostics.reason_name,
        (int)s_reset_diagnostics.reason,
        s_reset_diagnostics.boot_id,
        s_reset_diagnostics.uptime_ms
    );
    ESP_LOGI(
        TAG,
        "RESET_REASON reason=%s enum=%d uptime_ms=%llu boot_id=%u",
        s_reset_diagnostics.reason_name,
        (int)s_reset_diagnostics.reason,
        (unsigned long long)s_reset_diagnostics.uptime_ms,
        (unsigned)s_reset_diagnostics.boot_id
    );
}


void app_startup_log_success(void)
{
    app_startup_log_reset_reason();
#if APP_DISPLAY_ENABLED
    ESP_LOGI(TAG, "Display: OK");
#else
    ESP_LOGW(TAG, "Display: SKIPPED");
#endif

    hw_module_info_t sd_card = {0};
    const bool storage_ready =
        hw_inventory_get(HW_MODULE_SD_CARD, &sd_card) &&
        sd_card.state == HW_STATE_PRESENT;
    if (storage_ready) {
        ESP_LOGI(TAG, "SD card: OK");
        ESP_LOGI(TAG, "Storage manager: OK");
        ESP_LOGI(TAG, "Storage service: OK");
        ESP_LOGI(TAG, "Object recovery: OK");
        ESP_LOGI(TAG, "Object store: OK");
    } else {
        ESP_LOGW(TAG, "SD card: %s", hw_inventory_state_name(sd_card.state));
        ESP_LOGW(TAG, "Storage manager: SKIPPED");
        ESP_LOGI(TAG, "Storage service: started with storage unavailable");
        ESP_LOGW(TAG, "Object recovery: SKIPPED");
        ESP_LOGW(TAG, "Object store: SKIPPED");
    }

    ESP_LOGI(TAG, "Object service: OK");
#if APP_KEYPAD_ENABLED
    ESP_LOGI(TAG, "Keypad: OK");
#else
    ESP_LOGW(TAG, "Keypad: SKIPPED");
#endif

    if (
        wifi_manager_is_setup_mode()
    ) {
        ESP_LOGI(
            TAG,
            "Wi-Fi mode: SETUP AP"
        );

        ESP_LOGI(
            TAG,
            "Setup page: LOCAL QR FLOW"
        );
    } else {
        ESP_LOGI(
            TAG,
            "Wi-Fi mode: STA"
        );
    }

    ESP_LOGI(TAG, "Web server: OK");

    if (
        seismic_acquisition_is_running()
    ) {
        ESP_LOGI(
            TAG,
            "Seismic acquisition: %s RUNNING",
            seismic_acquisition_source_name()
        );
    } else {
        const esp_err_t acquisition_error =
            seismic_acquisition_last_error();
        if (acquisition_error == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Seismic acquisition: AD7606 NOT STARTED (starts when Measurements is opened)"
            );
        } else {
            ESP_LOGW(
                TAG,
                "Seismic acquisition: AD7606 FAILED: %s",
                esp_err_to_name(acquisition_error)
            );
        }
    }

    ESP_LOGI(TAG, "UI: OK");

    ESP_LOGI(
        TAG,
        "Hardware bring-up completed"
    );
}
