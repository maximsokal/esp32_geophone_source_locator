#pragma once

#include <stddef.h>
#include <string.h>

static inline size_t strlcpy(char *destination, const char *source, size_t size)
{
    const size_t source_length = strlen(source);
    if (size != 0U) {
        const size_t copy_length = source_length < size - 1U
            ? source_length
            : size - 1U;
        memcpy(destination, source, copy_length);
        destination[copy_length] = '\0';
    }
    return source_length;
}

static inline void web_wifi_input_log_stub(
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, ...) web_wifi_input_log_stub((tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) web_wifi_input_log_stub((tag), __VA_ARGS__)
#define ESP_LOGE(tag, ...) web_wifi_input_log_stub((tag), __VA_ARGS__)
