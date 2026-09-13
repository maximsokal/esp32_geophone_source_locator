#pragma once

#include <stdint.h>

uint32_t esp_log_timestamp(void);

static inline void power_test_log_stub(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) power_test_log_stub(__VA_ARGS__)
#define ESP_LOGW(...) power_test_log_stub(__VA_ARGS__)
#define ESP_LOGE(...) power_test_log_stub(__VA_ARGS__)
