#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

typedef enum {
    WIFI_MODE_STA = 1,
    WIFI_MODE_APSTA = 2,
} wifi_mode_t;

typedef enum {
    WIFI_IF_AP = 0,
    WIFI_IF_STA = 1,
} wifi_interface_t;

typedef enum {
    WIFI_PS_MIN_MODEM = 1,
} wifi_ps_type_t;

typedef enum {
    WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

typedef struct {
    struct {
        uint8_t ssid[33];
        uint8_t password[65];
        uint8_t ssid_len;
        uint8_t channel;
        uint8_t max_connection;
        wifi_auth_mode_t authmode;
    } ap;
} wifi_config_t;

static inline size_t strlcpy(char *destination, const char *source, size_t size)
{
    const size_t source_length = strlen(source);
    if (size != 0U) {
        const size_t copy_length = source_length < size - 1U
            ? source_length
            : size - 1U;
        memcpy(destination, source, copy_length);
        destination[copy_length] = '\0';
    }
    return source_length;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface_id, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
