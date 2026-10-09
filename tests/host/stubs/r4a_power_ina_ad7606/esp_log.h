#pragma once

static inline void r4a_test_log_stub(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) r4a_test_log_stub(__VA_ARGS__)
#define ESP_LOGW(...) r4a_test_log_stub(__VA_ARGS__)
#define ESP_LOGE(...) r4a_test_log_stub(__VA_ARGS__)
