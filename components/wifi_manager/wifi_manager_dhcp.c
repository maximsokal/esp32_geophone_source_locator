#include "wifi_manager_dhcp.h"

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#include "esp_log.h"
#include "esp_netif.h"


static const char *TAG =
    "WIFI_DHCP";


#if APP_WIFI_LOCAL_ONLY_ENABLED && defined(CONFIG_LWIP_DHCPS_ADD_DNS)
#error "Local-only Wi-Fi requires CONFIG_LWIP_DHCPS_ADD_DNS to be disabled"
#endif


esp_err_t wifi_manager_dhcp_configure(
    esp_netif_t *ap_netif
)
{
    if (ap_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


#if APP_WIFI_LOCAL_ONLY_ENABLED
    uint8_t router_enable =
        0;


    esp_err_t err =
        esp_netif_dhcps_option(
            ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
            &router_enable,
            sizeof(router_enable)
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
    ESP_LOGI(
        TAG,
        "DHCP default route advertisement enabled"
    );
#endif


    return ESP_OK;
}
