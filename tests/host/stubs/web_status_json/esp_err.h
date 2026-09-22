#pragma once

#include <stdint.h>

typedef int32_t esp_err_t;

#define ESP_OK                ((esp_err_t)0)
#define ESP_FAIL              ((esp_err_t)-1)
#define ESP_ERR_NO_MEM        ((esp_err_t)-2)
#define ESP_ERR_INVALID_ARG   ((esp_err_t)-3)
#define ESP_ERR_INVALID_STATE ((esp_err_t)-4)
#define ESP_ERR_NOT_FOUND     ((esp_err_t)-5)

static inline const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        default: return "ESP_ERR_UNKNOWN";
    }
}
