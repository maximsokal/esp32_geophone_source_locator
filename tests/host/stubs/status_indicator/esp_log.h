#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The test translation unit owns the buffers: the last formatted line, and every line so far. */
extern char host_esp_log_last[256];
extern char host_esp_log_all[4096];
/* While non-zero the code under test runs on a small-stack task (FreeRTOS "Tmr Svc"). */
extern int host_esp_log_forbidden;
extern unsigned host_esp_log_violations;

static inline void host_esp_log_sink(
    const char *tag,
    const char *format,
    ...
)
{
    va_list args;
    (void)tag;
    size_t used;
    if (host_esp_log_forbidden != 0) {
        ++host_esp_log_violations;
    }
    va_start(args, format);
    (void)vsnprintf(host_esp_log_last, sizeof(host_esp_log_last), format, args);
    va_end(args);
    used = strlen(host_esp_log_all);
    if (used + strlen(host_esp_log_last) + 2U < sizeof(host_esp_log_all)) {
        strcat(host_esp_log_all, host_esp_log_last);
        strcat(host_esp_log_all, "\n");
    }
}

#define ESP_LOGI(...) host_esp_log_sink(__VA_ARGS__)
#define ESP_LOGW(...) host_esp_log_sink(__VA_ARGS__)
#define ESP_LOGE(...) host_esp_log_sink(__VA_ARGS__)
