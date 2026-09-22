#pragma once

#include <stdarg.h>

static inline void object_store_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
    va_list args;
    va_start(args, format);
    va_end(args);
}

#define ESP_LOGI(...) object_store_test_log(__VA_ARGS__)
#define ESP_LOGW(...) object_store_test_log(__VA_ARGS__)
#define ESP_LOGE(...) object_store_test_log(__VA_ARGS__)
