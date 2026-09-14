#pragma once

static inline void ina3221_test_log(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) ina3221_test_log(__VA_ARGS__)
#define ESP_LOGW(...) ina3221_test_log(__VA_ARGS__)
#define ESP_LOGE(...) ina3221_test_log(__VA_ARGS__)
