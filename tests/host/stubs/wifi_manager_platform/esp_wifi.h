#pragma once

#include "esp_err.h"

typedef struct {
    int static_rx_buf_num;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ 0 })
#define WIFI_STORAGE_FLASH 1

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
