#pragma once

#include <assert.h>

typedef int esp_err_t;

#define ESP_OK                         0
#define ESP_FAIL                      -1
#define ESP_ERR_INVALID_STATE       0x103
#define ESP_ERR_NVS_NO_FREE_PAGES   0x1103
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1104

static inline const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
        case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
        default: return "ESP_ERR";
    }
}

#define ESP_ERROR_CHECK(expression) \
    do { assert((expression) == ESP_OK); } while (0)
