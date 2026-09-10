#pragma once

static inline void seismic_recorder_host_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, format, ...) seismic_recorder_host_log(tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) seismic_recorder_host_log(tag, format, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) seismic_recorder_host_log(tag, format, ##__VA_ARGS__)
