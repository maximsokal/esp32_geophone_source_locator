#pragma once

void startup_status_test_log(
    char level,
    const char *tag,
    const char *format,
    ...
);

#define ESP_LOGI(tag, ...) startup_status_test_log('I', (tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) startup_status_test_log('W', (tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) startup_status_test_log('E', (tag), __VA_ARGS__)
