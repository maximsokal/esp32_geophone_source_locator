#pragma once

static inline void power_output_test_log_stub(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) power_output_test_log_stub(__VA_ARGS__)
#define ESP_LOGW(...) power_output_test_log_stub(__VA_ARGS__)
#define ESP_LOGE(...) power_output_test_log_stub(__VA_ARGS__)
