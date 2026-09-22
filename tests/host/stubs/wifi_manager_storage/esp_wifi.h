#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"

#define WIFI_IF_STA 0

typedef struct {
    struct {
        uint8_t ssid[33];
        uint8_t password[65];
    } sta;
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

esp_err_t esp_wifi_get_config(int interface_id, wifi_config_t *config);
esp_err_t esp_wifi_set_config(int interface_id, const wifi_config_t *config);
