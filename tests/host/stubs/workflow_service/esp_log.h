#pragma once

static inline void workflow_test_log_noop(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, ...) workflow_test_log_noop((tag), __VA_ARGS__)
