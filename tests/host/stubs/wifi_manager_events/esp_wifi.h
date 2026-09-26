#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_REASON_UNSPECIFIED = 1,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
    WIFI_REASON_BEACON_TIMEOUT = 200,
    WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_AUTH_FAIL = 202,
    WIFI_REASON_HANDSHAKE_TIMEOUT = 204
} wifi_err_reason_t;

typedef struct {
    uint8_t mac[6];
} wifi_ap_record_t;

typedef struct {
    uint8_t mac[6];
} wifi_event_ap_staconnected_t;

typedef struct {
    wifi_err_reason_t reason;
} wifi_event_sta_disconnected_t;

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
