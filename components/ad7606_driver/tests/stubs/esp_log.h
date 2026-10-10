#pragma once

#ifdef AD7606_DRIVER_HOST_TEST
void ad7606_test_log(const char *tag, const char *format, ...);

#define ESP_LOGI(...) ad7606_test_log(__VA_ARGS__)
#define ESP_LOGW(...) ad7606_test_log(__VA_ARGS__)
#define ESP_LOGE(...) ad7606_test_log(__VA_ARGS__)
#else
#include <stdarg.h>

static inline void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#endif
