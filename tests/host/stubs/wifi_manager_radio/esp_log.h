#pragma once

static inline void wifi_manager_radio_log_stub(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, ...) wifi_manager_radio_log_stub((tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) wifi_manager_radio_log_stub((tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) wifi_manager_radio_log_stub((tag), __VA_ARGS__)
