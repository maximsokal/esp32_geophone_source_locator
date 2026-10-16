#pragma once

void app_storage_boot_ui_test_log(
    const char *level,
    const char *tag,
    const char *format,
    ...
);
#define ESP_LOGW(tag, ...) app_storage_boot_ui_test_log("W", tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) app_storage_boot_ui_test_log("E", tag, __VA_ARGS__)
