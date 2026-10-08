#pragma once

static inline void esp_log_stub(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#ifdef APP_STORAGE_BOOT_TEST_LOG_CAPTURE
void app_storage_boot_test_log_capture(const char *tag, const char *format, ...);
#define ESP_LOGI(...) esp_log_stub(__VA_ARGS__)
#define ESP_LOGW(...) app_storage_boot_test_log_capture(__VA_ARGS__)
#else
#define ESP_LOGI(...) esp_log_stub(__VA_ARGS__)
#define ESP_LOGW(...) esp_log_stub(__VA_ARGS__)
#endif
#define ESP_LOGE(...) esp_log_stub(__VA_ARGS__)
