#pragma once

#include "esp_err.h"
#include "esp_netif.h"


esp_err_t wifi_manager_dhcp_configure(
    esp_netif_t *ap_netif
);
