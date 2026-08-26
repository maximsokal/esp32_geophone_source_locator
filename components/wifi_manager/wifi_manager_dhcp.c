#include "wifi_manager_dhcp.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"

#include "esp_log.h"
#include "esp_netif.h"


static const char *TAG =
    "WIFI_DHCP";


static char s_captive_portal_uri[] =
    "http://192.168.4.1/";


#if APP_WIFI_LOCAL_ONLY_ENABLED && defined(CONFIG_LWIP_DHCPS_ADD_DNS)
#error "Local-only Wi-Fi requires CONFIG_LWIP_DHCPS_ADD_DNS to be disabled"
#endif


static esp_err_t wifi_manager_dhcp_set_router(
    esp_netif_t *ap_netif,
    uint8_t enabled
)
{
    return
        esp_netif_dhcps_option(
            ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
            &enabled,
            sizeof(enabled)
        );
}


static esp_err_t wifi_manager_dhcp_set_captive_portal(
    esp_netif_t *ap_netif
)
{
    return
        esp_netif_dhcps_option(
            ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_CAPTIVEPORTAL_URI,
            s_captive_portal_uri,
            strlen(s_captive_portal_uri)
        );
}


esp_err_t wifi_manager_dhcp_configure(
    esp_netif_t *ap_netif
)
{
    if (ap_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


#if APP_WIFI_LOCAL_ONLY_ENABLED
    esp_err_t err =
        wifi_manager_dhcp_set_router(
            ap_netif,
            0
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to disable DHCP default route: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "DHCP default route advertisement disabled"
    );

    ESP_LOGI(
        TAG,
        "DHCP DNS advertisement disabled"
    );
#else
    esp_err_t err =
        wifi_manager_dhcp_set_router(
            ap_netif,
            1
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to enable DHCP default route: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "DHCP default route advertisement enabled"
    );

    err =
        wifi_manager_dhcp_set_captive_portal(
            ap_netif
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to advertise captive portal URI: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "DHCP captive portal URI: %s",
        s_captive_portal_uri
    );
#endif


    return ESP_OK;
}
