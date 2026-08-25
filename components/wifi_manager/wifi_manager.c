#include "wifi_manager.h"

#include <string.h>

#include "app_config.h"
#include "wifi_manager_dhcp.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"


static const char *TAG = "WIFI_MANAGER";


static bool s_initialized = false;
static bool s_running = false;

static esp_netif_t *s_ap_netif = NULL;


static esp_err_t wifi_manager_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (
        err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        err = nvs_flash_init();
    }

    return err;
}


esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }


    ESP_ERROR_CHECK(
        wifi_manager_init_nvs()
    );


    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    esp_err_t err =
        esp_event_loop_create_default();

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        return err;
    }


    s_ap_netif =
        esp_netif_create_default_wifi_ap();

    if (s_ap_netif == NULL) {
        return ESP_FAIL;
    }


    ESP_ERROR_CHECK(
        wifi_manager_dhcp_configure(
            s_ap_netif
        )
    );


    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();


    ESP_ERROR_CHECK(
        esp_wifi_init(
            &wifi_init_config
        )
    );


    wifi_config_t ap_config = {
        .ap = {
            .ssid = APP_WIFI_AP_SSID,
            .password = APP_WIFI_AP_PASSWORD,

            .ssid_len = 0,

            .channel = 1,

            .authmode = WIFI_AUTH_WPA2_PSK,

            .max_connection = 4,

            .pmf_cfg = {
                .required = false,
            },
        },
    };


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_AP
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &ap_config
        )
    );


    s_initialized = true;


    ESP_LOGI(
        TAG,
        "Initialized"
    );


    return ESP_OK;
}


esp_err_t wifi_manager_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    if (s_running) {
        return ESP_OK;
    }


    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    s_running = true;


    ESP_LOGI(
        TAG,
        "SoftAP started"
    );

    ESP_LOGI(
        TAG,
        "SSID: %s",
        APP_WIFI_AP_SSID
    );

    ESP_LOGI(
        TAG,
        "Address: 192.168.4.1"
    );


    return ESP_OK;
}


esp_err_t wifi_manager_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }


    ESP_ERROR_CHECK(
        esp_wifi_stop()
    );


    s_running = false;


    ESP_LOGI(
        TAG,
        "SoftAP stopped"
    );


    return ESP_OK;
}


bool wifi_manager_is_running(void)
{
    return s_running;
}
