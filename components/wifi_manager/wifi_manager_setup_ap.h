#pragma once

#include "esp_err.h"
#include "esp_netif.h"


esp_err_t wifi_manager_setup_ap_start(
    esp_netif_t *ap_netif
);
