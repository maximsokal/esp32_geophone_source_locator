#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    unsigned marker;
} esp_netif_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
