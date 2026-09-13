#pragma once

#include <stdarg.h>

static inline void ui_event_dispatcher_test_log(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) ui_event_dispatcher_test_log(__VA_ARGS__)
#define ESP_LOGW(...) ui_event_dispatcher_test_log(__VA_ARGS__)
#define ESP_LOGE(...) ui_event_dispatcher_test_log(__VA_ARGS__)
