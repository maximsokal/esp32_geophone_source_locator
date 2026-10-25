#pragma once

#include <stdarg.h>
#include <stdio.h>

/* The test translation unit owns the buffer; it holds the last formatted line. */
extern char host_esp_log_last[256];

static inline void host_esp_log_sink(
    const char *tag,
    const char *format,
    ...
)
{
    va_list args;
    (void)tag;
    va_start(args, format);
    (void)vsnprintf(host_esp_log_last, sizeof(host_esp_log_last), format, args);
    va_end(args);
}

#define ESP_LOGI(...) host_esp_log_sink(__VA_ARGS__)
#define ESP_LOGW(...) host_esp_log_sink(__VA_ARGS__)
#define ESP_LOGE(...) host_esp_log_sink(__VA_ARGS__)
