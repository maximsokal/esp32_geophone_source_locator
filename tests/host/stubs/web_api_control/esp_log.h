#pragma once

static inline void web_api_control_test_log(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, ...) web_api_control_test_log((tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) web_api_control_test_log((tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) web_api_control_test_log((tag), __VA_ARGS__)
