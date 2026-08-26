#include "wifi_manager_platform.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"


static esp_netif_t *s_ap_netif =
    NULL;


static esp_err_t wifi_manager_platform_init_nvs(void)
{
    esp_err_t err =
        nvs_flash_init();

    if (
        err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        err =
            nvs_flash_init();
    }

    return err;
}


esp_err_t wifi_manager_platform_init(void)
{
    esp_err_t err =
        wifi_manager_platform_init_nvs();

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_netif_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_event_loop_create_default();

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        return err;
    }

    if (
        esp_netif_create_default_wifi_sta() ==
        NULL
    ) {
        return ESP_FAIL;
    }

    s_ap_netif =
        esp_netif_create_default_wifi_ap();

    if (s_ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t config =
        WIFI_INIT_CONFIG_DEFAULT();

    err =
        esp_wifi_init(
            &config
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        esp_wifi_set_storage(
            WIFI_STORAGE_FLASH
        );
}


esp_netif_t *wifi_manager_platform_ap_netif(void)
{
    return s_ap_netif;
}
