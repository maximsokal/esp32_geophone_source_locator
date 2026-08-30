#include "wifi_manager_mdns.h"

#include "app_config.h"

#include "esp_log.h"
#include "mdns.h"


static const char *TAG =
    "WIFI_MDNS";


static bool s_ready =
    false;


esp_err_t wifi_manager_mdns_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err =
        mdns_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        mdns_hostname_set(
            APP_MDNS_HOSTNAME
        );

    if (err != ESP_OK) {
        mdns_free();
        return err;
    }

    err =
        mdns_instance_name_set(
            APP_MDNS_INSTANCE_NAME
        );

    if (err != ESP_OK) {
        mdns_free();
        return err;
    }

    err =
        mdns_service_add(
            NULL,
            "_https",
            "_tcp",
            APP_WEB_HTTPS_PORT,
            NULL,
            0
        );

    if (err != ESP_OK) {
        mdns_free();
        return err;
    }

    s_ready =
        true;

    ESP_LOGI(
        TAG,
        "mDNS ready: %s",
        APP_MDNS_WEB_URL
    );

    return ESP_OK;
}


void wifi_manager_mdns_stop(void)
{
    if (!s_ready) {
        return;
    }

    mdns_free();
    s_ready = false;

    ESP_LOGI(
        TAG,
        "mDNS stopped"
    );
}


bool wifi_manager_mdns_is_ready(void)
{
    return s_ready;
}
