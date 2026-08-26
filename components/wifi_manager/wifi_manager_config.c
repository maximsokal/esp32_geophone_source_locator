#include "wifi_manager.h"

#include "wifi_manager_credentials.h"
#include "wifi_manager_profile.h"
#include "wifi_manager_runtime.h"

#include "esp_log.h"


static const char *TAG =
    "WIFI_MANAGER";


esp_err_t wifi_manager_save_sta_credentials(
    const char *ssid,
    const char *password
)
{
    esp_err_t err =
        wifi_manager_credentials_save(
            ssid,
            password
        );

    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6];

    if (
        wifi_manager_runtime_setup_client_mac(
            mac
        ) == ESP_OK
    ) {
        esp_err_t profile_err =
            wifi_manager_profile_remember_client(
                mac
            );

        if (profile_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to remember setup client: %s",
                esp_err_to_name(profile_err)
            );
        }
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi STA credentials saved"
    );

    return ESP_OK;
}


esp_err_t wifi_manager_forget_sta_credentials(void)
{
    esp_err_t err =
        wifi_manager_credentials_clear();

    if (err != ESP_OK) {
        return err;
    }

    esp_err_t profile_err =
        wifi_manager_profile_clear();

    if (profile_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to clear Wi-Fi profile: %s",
            esp_err_to_name(profile_err)
        );
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi credentials forgotten"
    );

    return ESP_OK;
}


esp_err_t wifi_manager_get_saved_ssid(
    char *ssid,
    size_t ssid_size
)
{
    return
        wifi_manager_credentials_get_ssid(
            ssid,
            ssid_size
        );
}
