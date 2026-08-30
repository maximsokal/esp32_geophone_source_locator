#include "wifi_manager.h"

#include "wifi_manager_credentials.h"
#include "wifi_manager_internal.h"
#include "wifi_manager_mdns.h"
#include "wifi_manager_platform.h"
#include "wifi_manager_setup_ap.h"
#include "wifi_manager_sta.h"

#include "esp_check.h"
#include "esp_log.h"


static const char *TAG =
    "WIFI_MANAGER";


static bool s_initialized =
    false;

static bool s_running =
    false;

static bool s_setup_mode =
    false;


bool wifi_manager_internal_initialized(void)
{
    return s_initialized;
}


void wifi_manager_internal_set_running(
    bool running
)
{
    s_running =
        running;
}


void wifi_manager_internal_set_setup_mode(
    bool setup_mode
)
{
    s_setup_mode =
        setup_mode;
}


bool wifi_manager_internal_setup_mode(void)
{
    return s_setup_mode;
}


esp_err_t wifi_manager_internal_start_setup_mode(void)
{
    esp_err_t err =
        wifi_manager_setup_ap_start(
            wifi_manager_platform_ap_netif()
        );

    if (err == ESP_OK) {
        s_setup_mode =
            true;

        ESP_LOGI(
            TAG,
            "Setup mode: local captive portal"
        );
    }

    return err;
}


esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        wifi_manager_platform_init(),
        TAG,
        "Wi-Fi platform initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        wifi_manager_sta_init(),
        TAG,
        "Wi-Fi STA events initialization failed"
    );

    /*
     * mDNS is a convenience layer on top of the normal IP path. Keep Wi-Fi
     * usable through its DHCP address even if discovery cannot start.
     */
    esp_err_t mdns_err =
        wifi_manager_mdns_init();

    if (mdns_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "mDNS unavailable: %s; numeric IP remains available",
            esp_err_to_name(mdns_err)
        );
    }

    s_initialized =
        true;

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

    bool has_credentials =
        false;

    ESP_RETURN_ON_ERROR(
        wifi_manager_credentials_has_saved(
            &has_credentials
        ),
        TAG,
        "Failed to inspect saved Wi-Fi credentials"
    );

    esp_err_t err;

    if (has_credentials) {
        s_setup_mode =
            false;

        err =
            wifi_manager_sta_start();

        if (err != ESP_OK) {
            return err;
        }

        ESP_LOGI(
            TAG,
            "Normal mode: Wi-Fi STA"
        );
    } else {
        err =
            wifi_manager_internal_start_setup_mode();

        if (err != ESP_OK) {
            return err;
        }
    }

    s_running =
        true;

    return ESP_OK;
}


bool wifi_manager_is_running(void)
{
    return s_running;
}


bool wifi_manager_is_setup_mode(void)
{
    return
        s_running &&
        s_setup_mode;
}
