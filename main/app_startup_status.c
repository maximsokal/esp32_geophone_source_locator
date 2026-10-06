#include "app_startup_status.h"

#include "hw_inventory.h"
#include "seismic_acquisition.h"
#include "wifi_manager.h"

#include "esp_log.h"


static const char *TAG =
    "APP";


void app_startup_log_success(void)
{
    ESP_LOGI(TAG, "Display: OK");

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
    ESP_LOGI(TAG, "Keypad: OK");

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
