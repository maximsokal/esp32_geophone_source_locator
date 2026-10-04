#pragma once

#include <stdarg.h>

#ifdef KEYPAD_TEST_LOG_CAPTURE
void keypad_test_log_capture(const char *tag, const char *format, va_list args);
#endif

static inline void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
#ifdef KEYPAD_TEST_LOG_CAPTURE
    va_list args;
    va_start(args, format);
    keypad_test_log_capture(tag, format, args);
    va_end(args);
#endif
}

#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
/* DEBUG remains hidden in the host capture; INFO transition diagnostics are captured. */
#define ESP_LOGD(...) ((void)0)
