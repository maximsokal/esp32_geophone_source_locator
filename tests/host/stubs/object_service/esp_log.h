#pragma once

static inline void host_esp_log_sink(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) host_esp_log_sink(__VA_ARGS__)
#define ESP_LOGW(...) host_esp_log_sink(__VA_ARGS__)
#define ESP_LOGE(...) host_esp_log_sink(__VA_ARGS__)
