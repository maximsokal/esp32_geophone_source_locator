#pragma once

#include <stdint.h>

uint32_t esp_log_timestamp(void);

#define ESP_LOGI(tag, format, ...) ((void)0)
#define ESP_LOGW(tag, format, ...) ((void)0)
#define ESP_LOGE(tag, format, ...) ((void)0)
