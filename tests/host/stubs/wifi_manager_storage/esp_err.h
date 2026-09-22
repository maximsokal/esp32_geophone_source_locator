#pragma once

typedef int esp_err_t;

#define ESP_OK                       0
#define ESP_FAIL                    -1
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_SIZE      0x106
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NVS_NOT_FOUND     0x1101
#define ESP_ERR_NVS_INVALID_LENGTH 0x1102

static inline const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
        default: return "ESP_ERR";
    }
}
