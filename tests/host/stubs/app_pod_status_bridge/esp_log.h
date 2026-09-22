#pragma once

static inline void app_pod_status_bridge_test_log(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, ...) app_pod_status_bridge_test_log((tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) app_pod_status_bridge_test_log((tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) app_pod_status_bridge_test_log((tag), __VA_ARGS__)
