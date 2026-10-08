#pragma once

#if defined(APP_POD_STATUS_BRIDGE_TEST_LOG_CAPTURE)

/* The bridge runtime test captures log levels to assert rate-limited warnings. */
void app_pod_status_bridge_test_log(
    char level,
    const char *tag,
    const char *format,
    ...);

#define ESP_LOGI(tag, ...) app_pod_status_bridge_test_log('I', (tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) app_pod_status_bridge_test_log('W', (tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) app_pod_status_bridge_test_log('E', (tag), __VA_ARGS__)

#else

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

#endif
