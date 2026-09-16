#pragma once

#include "esp_err.h"
#include "esp_netif.h"


esp_err_t wifi_manager_platform_init(void);

esp_netif_t *wifi_manager_platform_sta_netif(void);

esp_netif_t *wifi_manager_platform_ap_netif(void);
