#pragma once

#include <stdint.h>

typedef int32_t esp_err_t;

#define ESP_OK 0

const char *esp_err_to_name(esp_err_t error);
