#pragma once

static inline void seismic_recorder_host_log(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) seismic_recorder_host_log(__VA_ARGS__)
#define ESP_LOGW(...) seismic_recorder_host_log(__VA_ARGS__)
#define ESP_LOGE(...) seismic_recorder_host_log(__VA_ARGS__)
