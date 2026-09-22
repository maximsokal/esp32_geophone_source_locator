#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t mac[6];
} wifi_ap_record_t;

typedef struct {
    uint8_t mac[6];
} wifi_event_ap_staconnected_t;

typedef struct {
    uint8_t mac[6];
} wifi_sta_info_t;

typedef struct {
    uint16_t num;
    wifi_sta_info_t sta[8];
} wifi_sta_list_t;

esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *sta_list);
